#define NOMINMAX
#include "AudioCDCopier.h"
#include "AccurateRip.h"
#include "InterruptHandler.h"
#include "MenuHelpers.h"
#include <iostream>
#include <fstream>
#include <iomanip>
// ... other includes as needed
// ============================================================================
// Disc Fingerprinting
// ============================================================================

bool AudioCDCopier::GenerateDiscFingerprint(const DiscInfo& disc, DiscFingerprint& fingerprint) {
	if (disc.tracks.empty()) {
		std::cerr << "Error: No tracks to fingerprint\n";
		return false;
	}

	std::cout << "\n=== Generating Disc Fingerprint ===\n";
	fingerprint = DiscFingerprint{};

	bool cddbOk = CalculateCDDBId(disc, fingerprint.cddb);
	bool mbOk = CalculateMusicBrainzId(disc, fingerprint.musicBrainz);
	bool arOk = CalculateAccurateRipId(disc, fingerprint.accurateRip);

	if (!disc.rawSectors.empty()) {
		CalculateAudioFingerprint(disc, fingerprint.audio);
	}

	std::ostringstream toc;
	toc << "Tracks: " << disc.tracks.size() << " | ";
	for (const auto& t : disc.tracks) {
		toc << t.startLBA << " ";
	}
	toc << "| Lead-out: " << disc.leadOutLBA;
	fingerprint.tocString = toc.str();

	time_t now = time(nullptr);
	char timeBuf[64];
	struct tm timeinfo;
	localtime_s(&timeinfo, &now);
	strftime(timeBuf, sizeof(timeBuf), "%Y-%m-%dT%H:%M:%S", &timeinfo);
	fingerprint.generationTime = timeBuf;

	fingerprint.isValid = cddbOk || mbOk || arOk;

	if (fingerprint.isValid) {
		PrintDiscFingerprint(fingerprint);
	}

	return fingerprint.isValid;
}

bool AudioCDCopier::CalculateCDDBId(const DiscInfo& disc, CDDBFingerprint& cddb) {
	if (disc.tracks.empty()) return false;

	cddb.trackCount = static_cast<int>(disc.tracks.size());
	cddb.trackOffsets.clear();

	int checksumTotal = 0;

	for (const auto& track : disc.tracks) {
		int frames = static_cast<int>(track.startLBA) + 150;
		int seconds = frames / 75;

		cddb.trackOffsets.push_back(frames);
		checksumTotal += CDDBSum(seconds);
	}

	int leadOutFrames = static_cast<int>(disc.leadOutLBA) + 150;
	int firstTrackFrames = static_cast<int>(disc.tracks[0].startLBA) + 150;
	cddb.totalSeconds = (leadOutFrames - firstTrackFrames) / 75;

	uint32_t checksum = static_cast<uint32_t>(checksumTotal % 255);
	cddb.discId = (checksum << 24) |
		(static_cast<uint32_t>(cddb.totalSeconds) << 8) |
		static_cast<uint32_t>(cddb.trackCount);

	char hexBuf[16];
	snprintf(hexBuf, sizeof(hexBuf), "%08x", cddb.discId);
	cddb.discIdHex = hexBuf;

	std::cout << "  CDDB ID: " << cddb.discIdHex << "\n";
	return true;
}

int AudioCDCopier::CDDBSum(int n) {
	int sum = 0;
	while (n > 0) {
		sum += n % 10;
		n /= 10;
	}
	return sum;
}

bool AudioCDCopier::CalculateMusicBrainzId(const DiscInfo& disc, MusicBrainzFingerprint& mb) {
	if (disc.tracks.empty()) return false;

	mb.firstTrack = disc.tracks[0].trackNumber;
	mb.lastTrack = disc.tracks.back().trackNumber;
	mb.leadOutOffset = static_cast<int>(disc.leadOutLBA) + 150;

	mb.trackOffsets.clear();
	for (const auto& track : disc.tracks) {
		mb.trackOffsets.push_back(static_cast<int>(track.startLBA) + 150);
	}

	std::ostringstream tocStream;
	tocStream << std::hex << std::uppercase << std::setfill('0');

	tocStream << std::setw(2) << mb.firstTrack;
	tocStream << std::setw(2) << mb.lastTrack;
	tocStream << std::setw(8) << mb.leadOutOffset;

	for (int i = 0; i < 99; i++) {
		if (i < static_cast<int>(mb.trackOffsets.size())) {
			tocStream << std::setw(8) << mb.trackOffsets[i];
		}
		else {
			tocStream << std::setw(8) << 0;
		}
	}

	std::string tocString = tocStream.str();

	BYTE sha1Result[20];
	SHA1Hash(reinterpret_cast<const BYTE*>(tocString.c_str()),
		tocString.length(), sha1Result);

	mb.discId = Base64Encode(sha1Result, 20);

	for (char& c : mb.discId) {
		if (c == '+') c = '.';
		else if (c == '/') c = '_';
	}
	while (!mb.discId.empty() && mb.discId.back() == '=') {
		mb.discId.pop_back();
	}
	mb.discId += '-';

	std::cout << "  MusicBrainz ID: " << mb.discId << "\n";
	return true;
}

bool AudioCDCopier::CalculateAccurateRipId(const DiscInfo& disc, AccurateRipFingerprint& ar) {
	if (disc.tracks.empty()) return false;

	ar.trackCount = static_cast<int>(disc.tracks.size());
	ar.discId1 = 0;
	ar.discId2 = 0;

	int trackNum = 1;
	for (const auto& track : disc.tracks) {
		int offset = static_cast<int>(track.startLBA) + 150;
		ar.discId1 += static_cast<uint32_t>(offset);
		ar.discId2 += static_cast<uint32_t>(offset) * trackNum;
		trackNum++;
	}

	ar.discId2 += static_cast<uint32_t>(disc.leadOutLBA + 150) * (trackNum);

	CDDBFingerprint cddbTemp;
	CalculateCDDBId(disc, cddbTemp);
	ar.cddbDiscId = cddbTemp.discId;

	std::cout << "  AccurateRip ID1: " << std::hex << std::setfill('0')
		<< std::setw(8) << ar.discId1 << std::dec << "\n";
	std::cout << "  AccurateRip ID2: " << std::hex << std::setfill('0')
		<< std::setw(8) << ar.discId2 << std::dec << "\n";

	return true;
}

bool AudioCDCopier::CalculateAudioFingerprint(const DiscInfo& disc, AudioFingerprint& audio) {
	if (disc.rawSectors.empty()) return false;

	audio = AudioFingerprint{};
	audio.trackHashes.resize(disc.tracks.size(), 0);

	uint32_t audioHash = 2166136261u;
	uint32_t silenceHash = 2166136261u;
	int sampleCount = 0;

	size_t sectorIdx = 0;
	for (size_t trackIdx = 0; trackIdx < disc.tracks.size(); trackIdx++) {
		const auto& track = disc.tracks[trackIdx];
		if (!track.isAudio) continue;

		uint32_t trackHash = 2166136261u;
		DWORD trackSectors = track.endLBA - track.pregapLBA + 1;

		int sampleInterval = std::max(1, static_cast<int>(trackSectors / 100));

		for (DWORD i = 0; i < trackSectors && sectorIdx < disc.rawSectors.size(); i++, sectorIdx++) {
			if (i % sampleInterval != 0) continue;

			const auto& sector = disc.rawSectors[sectorIdx];
			if (sector.size() < AUDIO_SECTOR_SIZE) continue;

			for (int j = 0; j < AUDIO_SECTOR_SIZE; j += 16) {
				uint32_t sample = *reinterpret_cast<const uint32_t*>(sector.data() + j);

				audioHash ^= sample;
				audioHash *= 16777619u;

				if (sample == 0) {
					silenceHash ^= static_cast<uint32_t>(sectorIdx & 0xFFFFFFFF);
					silenceHash *= 16777619u;
				}

				trackHash ^= sample;
				trackHash *= 16777619u;
			}
			sampleCount++;
		}

		audio.trackHashes[trackIdx] = trackHash;
	}

	audio.audioHash = audioHash;
	audio.silenceProfile = silenceHash;
	audio.sampleCount = sampleCount;

	std::cout << "  Audio Hash: " << std::hex << std::setfill('0')
		<< std::setw(8) << audio.audioHash << std::dec << "\n";
	std::cout << "  Samples analyzed: " << audio.sampleCount << "\n";

	return true;
}

void AudioCDCopier::PrintDiscFingerprint(const DiscFingerprint& fingerprint) {
	std::cout << "\n" << std::string(50, '=') << "\n";
	std::cout << "         DISC FINGERPRINT REPORT\n";
	std::cout << std::string(50, '=') << "\n\n";

	std::cout << "=== CDDB/FreeDB ===\n";
	std::cout << "  Disc ID:      " << fingerprint.cddb.discIdHex << "\n";
	std::cout << "  Track Count:  " << fingerprint.cddb.trackCount << "\n";
	std::cout << "  Total Length: " << fingerprint.cddb.totalSeconds / 60 << ":"
		<< std::setfill('0') << std::setw(2) << fingerprint.cddb.totalSeconds % 60 << "\n";
	std::cout << "  Lookup URL:   " << fingerprint.GetCDDBUrl() << "\n";

	std::cout << "\n=== MusicBrainz ===\n";
	std::cout << "  Disc ID:      " << fingerprint.musicBrainz.discId << "\n";
	std::cout << "  Tracks:       " << fingerprint.musicBrainz.firstTrack << "-"
		<< fingerprint.musicBrainz.lastTrack << "\n";
	std::cout << "  Lead-out:     " << fingerprint.musicBrainz.leadOutOffset << " frames\n";
	std::cout << "  Lookup URL:   " << fingerprint.GetMusicBrainzUrl() << "\n";

	std::cout << "\n=== AccurateRip ===\n";
	std::cout << "  Disc ID 1:    " << std::hex << std::setfill('0')
		<< std::setw(8) << fingerprint.accurateRip.discId1 << std::dec << "\n";
	std::cout << "  Disc ID 2:    " << std::hex << std::setfill('0')
		<< std::setw(8) << fingerprint.accurateRip.discId2 << std::dec << "\n";
	std::cout << "  Database URL: " << fingerprint.GetAccurateRipUrl() << "\n";

	if (fingerprint.audio.sampleCount > 0) {
		std::cout << "\n=== Audio Content ===\n";
		std::cout << "  Audio Hash:   " << std::hex << std::setfill('0')
			<< std::setw(8) << fingerprint.audio.audioHash << std::dec << "\n";
		std::cout << "  Samples:      " << fingerprint.audio.sampleCount << "\n";
	}

	std::cout << "\n=== TOC Summary ===\n";
	std::cout << "  " << fingerprint.tocString << "\n";
	std::cout << "  Generated:    " << fingerprint.generationTime << "\n";
	std::cout << std::string(50, '=') << "\n";
}

bool AudioCDCopier::SaveDiscFingerprint(const DiscFingerprint& fingerprint, const std::wstring& filename) {
	std::ofstream file(filename);
	if (!file) return false;

	file << "# Disc Fingerprint\n";
	file << "# Generated: " << fingerprint.generationTime << "\n\n";
	file << "[CDDB]\nDiscID=" << fingerprint.cddb.discIdHex << "\n";
	file << "TrackCount=" << fingerprint.cddb.trackCount << "\n";
	file << "TotalSeconds=" << fingerprint.cddb.totalSeconds << "\n\n";
	file << "[MusicBrainz]\nDiscID=" << fingerprint.musicBrainz.discId << "\n\n";
	file << "[AccurateRip]\nDiscID1=" << std::hex << fingerprint.accurateRip.discId1 << "\n";
	file << "DiscID2=" << fingerprint.accurateRip.discId2 << std::dec << "\n";
	file.close();
	return true;
}

std::string AudioCDCopier::Base64Encode(const BYTE* data, size_t length) {
	static const char* chars = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
	std::string result;
	result.reserve(((length + 2) / 3) * 4);
	for (size_t i = 0; i < length; i += 3) {
		uint32_t n = static_cast<uint32_t>(data[i]) << 16;
		if (i + 1 < length) n |= static_cast<uint32_t>(data[i + 1]) << 8;
		if (i + 2 < length) n |= static_cast<uint32_t>(data[i + 2]);
		result += chars[(n >> 18) & 0x3F];
		result += chars[(n >> 12) & 0x3F];
		result += (i + 1 < length) ? chars[(n >> 6) & 0x3F] : '=';
		result += (i + 2 < length) ? chars[n & 0x3F] : '=';
	}
	return result;
}

void AudioCDCopier::SHA1Hash(const BYTE* data, size_t length, BYTE* output) {
	uint32_t h0 = 0x67452301, h1 = 0xEFCDAB89, h2 = 0x98BADCFE, h3 = 0x10325476, h4 = 0xC3D2E1F0;
	size_t newLen = length + 1;
	while (newLen % 64 != 56) newLen++;
	std::vector<BYTE> msg(newLen + 8, 0);
	memcpy(msg.data(), data, length);
	msg[length] = 0x80;
	uint64_t bitLen = static_cast<uint64_t>(length) * 8;
	for (int i = 0; i < 8; i++) msg[newLen + 7 - i] = static_cast<BYTE>(bitLen >> (i * 8));
	auto leftRotate = [](uint32_t x, int n) { return (x << n) | (x >> (32 - n)); };
	for (size_t chunk = 0; chunk < msg.size(); chunk += 64) {
		uint32_t w[80];
		for (int i = 0; i < 16; i++)
			w[i] = (static_cast<uint32_t>(msg[chunk + i * 4]) << 24) | (static_cast<uint32_t>(msg[chunk + i * 4 + 1]) << 16) |
			(static_cast<uint32_t>(msg[chunk + i * 4 + 2]) << 8) | static_cast<uint32_t>(msg[chunk + i * 4 + 3]);
		for (int i = 16; i < 80; i++) w[i] = leftRotate(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
		uint32_t a = h0, b = h1, c = h2, d = h3, e = h4;
		for (int i = 0; i < 80; i++) {
			uint32_t f, k;
			if (i < 20) { f = (b & c) | ((~b) & d); k = 0x5A827999; }
			else if (i < 40) { f = b ^ c ^ d; k = 0x6ED9EBA1; }
			else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDC; }
			else { f = b ^ c ^ d; k = 0xCA62C1D6; }
			uint32_t temp = leftRotate(a, 5) + f + e + k + w[i];
			e = d; d = c; c = leftRotate(b, 30); b = a; a = temp;
		}
		h0 += a; h1 += b; h2 += c; h3 += d; h4 += e;
	}
	uint32_t hashes[] = { h0, h1, h2, h3, h4 };
	for (int i = 0; i < 5; i++) {
		output[i * 4] = static_cast<BYTE>(hashes[i] >> 24);
		output[i * 4 + 1] = static_cast<BYTE>(hashes[i] >> 16);
		output[i * 4 + 2] = static_cast<BYTE>(hashes[i] >> 8);
		output[i * 4 + 3] = static_cast<BYTE>(hashes[i]);
	}
}

std::string DiscFingerprint::GetCDDBUrl() const {
	return "https://gnudb.org/cd/" + cddb.discIdHex;
}

std::string DiscFingerprint::GetMusicBrainzUrl() const {
	return "https://musicbrainz.org/cdtoc/" + musicBrainz.discId;
}

std::string DiscFingerprint::GetAccurateRipUrl() const {
	std::ostringstream url;
	url << "http://www.accuraterip.com/accuraterip/"
		<< std::hex << std::setfill('0')
		<< std::setw(1) << (accurateRip.discId1 & 0xF) << "/"
		<< std::setw(1) << ((accurateRip.discId1 >> 4) & 0xF) << "/"
		<< std::setw(1) << ((accurateRip.discId1 >> 8) & 0xF) << "/"
		<< "dBAR-" << std::setw(3) << std::dec << accurateRip.trackCount
		<< "-" << std::hex << std::setw(8) << accurateRip.discId1
		<< "-" << std::setw(8) << accurateRip.discId2
		<< "-" << std::setw(8) << accurateRip.cddbDiscId << ".bin";
	return url.str();
}