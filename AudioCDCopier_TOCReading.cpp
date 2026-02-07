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
// TOC Reading
// ============================================================================

bool AudioCDCopier::ReadTOC(DiscInfo& disc) {
	if (!ReadFullTOC(disc)) return false;

	std::cout << "\nScanning pregaps (slowing drive for accuracy)...\n";
	m_drive.SetSpeed(4);

	if (!disc.tracks.empty() && disc.tracks[0].isAudio) {
		DWORD track1Start = disc.tracks[0].startLBA;
		DWORD scanStart = 0;
		DWORD firstIndex0 = track1Start;
		bool foundIndex0 = false;
		int consecutiveIndex0 = 0;

		for (DWORD lba = scanStart; lba < track1Start; lba++) {
			int qTrack = 0, qIndex = -1;
			for (int attempt = 0; attempt < 3 && qIndex < 0; attempt++) {
				if (m_drive.ReadSectorQ(lba, qTrack, qIndex)) {
					if (qTrack == 1 && qIndex == 0) {
						if (!foundIndex0) { firstIndex0 = lba; foundIndex0 = true; consecutiveIndex0 = 1; }
						else { consecutiveIndex0++; }
						break;
					}
				}
			}
		}

		if (foundIndex0 && consecutiveIndex0 >= 3) {
			disc.tracks[0].pregapLBA = firstIndex0;
		}
		else {
			disc.tracks[0].pregapLBA = 0;
		}

		int frames = static_cast<int>(track1Start - disc.tracks[0].pregapLBA);
		std::cout << "  Track 1: " << frames / 75 << ":"
			<< std::setfill('0') << std::setw(2) << frames % 75 << std::setfill(' ')
			<< " (" << frames << " frames) pregap\n";
	}

	for (size_t i = 1; i < disc.tracks.size(); i++) {
		g_interrupt.CheckInterrupt();
		if (disc.selectedSession > 0 && disc.tracks[i].session != disc.selectedSession) continue;
		if (!disc.tracks[i].isAudio) {
			std::cout << "  Track " << disc.tracks[i].trackNumber << ": DATA track, skipping\n";
			continue;
		}

		DWORD trackStart = disc.tracks[i].startLBA;
		int targetTrack = disc.tracks[i].trackNumber;
		DWORD pregapStart = trackStart;
		DWORD scanStart = trackStart > 450 ? trackStart - 450 : 0;
		DWORD firstIndex0 = trackStart;
		bool foundAny = false;
		int consecutiveIndex0 = 0;

		for (DWORD lba = scanStart; lba < trackStart; lba++) {
			int qTrack = 0, qIndex = -1;
			for (int attempt = 0; attempt < 3 && qIndex < 0; attempt++) {
				if (m_drive.ReadSectorQ(lba, qTrack, qIndex)) {
					if (qTrack == targetTrack && qIndex == 0) {
						if (!foundAny) { firstIndex0 = lba; foundAny = true; consecutiveIndex0 = 1; }
						else { consecutiveIndex0++; }
						break;
					}
					else if (foundAny && qIndex == 1) {
						break;
					}
				}
			}
			if (foundAny && qIndex != 0 && consecutiveIndex0 < 3) {
				foundAny = false; consecutiveIndex0 = 0; firstIndex0 = trackStart;
			}
			if (foundAny && qIndex == 1 && consecutiveIndex0 >= 3) {
				break;
			}
		}

		if (foundAny && consecutiveIndex0 >= 3) pregapStart = firstIndex0;
		disc.tracks[i].pregapLBA = pregapStart;

		auto& prev = disc.tracks[i - 1];
		if (pregapStart > 0 && pregapStart - 1 < prev.endLBA) prev.endLBA = pregapStart - 1;

		int frames = static_cast<int>(trackStart - pregapStart);
		std::cout << "  Track " << targetTrack << ": " << frames / 75 << ":"
			<< std::setfill('0') << std::setw(2) << frames % 75 << std::setfill(' ')
			<< " (" << frames << " frames)\n";
	}

	m_drive.SetSpeed(0);
	std::cout << "\nTOC: " << disc.tracks.size() << " tracks\n";

	DetectHiddenTrack(disc);

	return true;
}

bool AudioCDCopier::ReadFullTOC(DiscInfo& disc) {
	BYTE sessionCdb[10] = { 0x43, 0x00, 1, 0, 0, 0, 0, 0, 12, 0 };
	std::vector<BYTE> sessionBuf(12);
	if (m_drive.SendSCSI(sessionCdb, 10, sessionBuf.data(), 12)) {
		if (sessionBuf[3] >= sessionBuf[2]) {
			disc.sessionCount = sessionBuf[3] - sessionBuf[2] + 1;
		}
	}

	BYTE tocCdb[10] = { 0x43, 0x00, 0, 0, 0, 0, 0, 0x03, 0x24, 0 };
	std::vector<BYTE> tocBuf(804);
	if (!m_drive.SendSCSI(tocCdb, 10, tocBuf.data(), 804)) return false;

	int tocLen = (tocBuf[0] << 8) | tocBuf[1];
	if (tocLen < 2) return false;

	int firstTrack = tocBuf[2];
	int lastTrack = tocBuf[3];
	int n = lastTrack - firstTrack + 1;
	if (n <= 0 || n > 99) return false;

	disc.tracks.clear();

	BYTE fullTocCdb[10] = { 0x43, 0x00, 2, 0, 0, 0, 0, 0x08, 0x00, 0 };
	std::vector<BYTE> fullToc(2048);
	bool hasFullToc = m_drive.SendSCSI(fullTocCdb, 10, fullToc.data(), 2048);

	std::vector<int> trackSession(100, 1);
	if (hasFullToc) {
		int fullTocLen = (fullToc[0] << 8) | fullToc[1];
		int endOffset = (fullTocLen + 2 < 2048) ? fullTocLen + 2 : 2048;
		BYTE* p = fullToc.data() + 4;
		BYTE* end = fullToc.data() + endOffset;
		while (p + 11 <= end) {
			int session = p[0];
			int point = p[3];
			if (point >= 1 && point <= 99) trackSession[point] = session;
			p += 11;
		}
	}

	for (int i = 0; i < n; i++) {
		BYTE* td = tocBuf.data() + 4 + i * 8;
		TrackInfo t = {};
		t.trackNumber = td[2];
		t.isAudio = (td[1] & 0x04) == 0;
		t.mode = t.isAudio ? 0 : 1;
		t.session = trackSession[t.trackNumber];
		t.startLBA = (static_cast<DWORD>(td[4]) << 24) |
			(static_cast<DWORD>(td[5]) << 16) |
			(static_cast<DWORD>(td[6]) << 8) |
			static_cast<DWORD>(td[7]);

		if (i < n - 1) {
			BYTE* nextTd = tocBuf.data() + 4 + (i + 1) * 8;
			DWORD nextLBA = (static_cast<DWORD>(nextTd[4]) << 24) |
				(static_cast<DWORD>(nextTd[5]) << 16) |
				(static_cast<DWORD>(nextTd[6]) << 8) |
				static_cast<DWORD>(nextTd[7]);  // ✅ FIXED
			t.endLBA = nextLBA - 1;
		}
		else {
			BYTE* leadOut = tocBuf.data() + 4 + n * 8;
			DWORD leadOutLBA = (static_cast<DWORD>(leadOut[4]) << 24) |
				(static_cast<DWORD>(leadOut[5]) << 16) |
				(static_cast<DWORD>(leadOut[6]) << 8) |
				static_cast<DWORD>(leadOut[7]);  // ✅ FIXED
			t.endLBA = leadOutLBA - 1;
		}
		t.pregapLBA = t.startLBA;
		disc.tracks.push_back(t);
	}

	BYTE* leadOut = tocBuf.data() + 4 + n * 8;
	disc.leadOutLBA = (static_cast<DWORD>(leadOut[4]) << 24) |
		(static_cast<DWORD>(leadOut[5]) << 16) |
		(static_cast<DWORD>(leadOut[6]) << 8) |
		static_cast<DWORD>(leadOut[7]);
	return true;
}

bool AudioCDCopier::ReadCDText(DiscInfo& disc) {
	BYTE cdb[10] = { 0x43, 0x00, 5, 0, 0, 0, 0, 0, 4, 0 };
	std::vector<BYTE> buf(4);
	if (!m_drive.SendSCSI(cdb, 10, buf.data(), 4)) return false;

	WORD dataLen = static_cast<WORD>((buf[0] << 8) | buf[1]);
	if (dataLen < 4) return false;

	buf.resize(dataLen + 2);
	cdb[7] = static_cast<BYTE>(((dataLen + 2) >> 8) & 0xFF);
	cdb[8] = static_cast<BYTE>((dataLen + 2) & 0xFF);
	if (!m_drive.SendSCSI(cdb, 10, buf.data(), dataLen + 2)) return false;

	disc.cdText.trackTitles.resize(disc.tracks.size());
	disc.cdText.trackArtists.resize(disc.tracks.size());

	BYTE* p = buf.data() + 4;
	BYTE* end = buf.data() + dataLen + 2;
	while (p + 18 <= end) {
		BYTE packType = p[0];
		BYTE trackNum = p[1] & 0x7F;
		std::string text(reinterpret_cast<char*>(&p[4]), 12);
		size_t nullPos = text.find('\0');
		if (nullPos != std::string::npos) text.resize(nullPos);

		if (!text.empty()) {
			if (packType == 0x80) {
				if (trackNum == 0) disc.cdText.albumTitle = text;
				else if (trackNum <= disc.tracks.size()) disc.cdText.trackTitles[trackNum - 1] = text;
			}
			else if (packType == 0x81) {
				if (trackNum == 0) disc.cdText.albumArtist = text;
				else if (trackNum <= disc.tracks.size()) disc.cdText.trackArtists[trackNum - 1] = text;
			}
		}
		p += 18;
	}
	return !disc.cdText.albumTitle.empty() || !disc.cdText.albumArtist.empty();
}

bool AudioCDCopier::ReadISRC(DiscInfo& disc) {
	std::cout << "\nReading ISRC codes...\n";

	for (auto& track : disc.tracks) {
		if (!track.isAudio) continue;

		BYTE cdb[10] = { 0x42, 0x02, 0x03, 0, 0, 0, static_cast<BYTE>(track.trackNumber), 0, 24, 0 };
		std::vector<BYTE> buf(24);

		if (m_drive.SendSCSI(cdb, 10, buf.data(), 24)) {
			if (buf[8] & 0x80) {
				std::string isrc;
				for (int i = 0; i < 12; i++) {
					char c = buf[9 + i];
					if (c >= '0' && c <= '9') isrc += c;
					else if (c >= 'A' && c <= 'Z') isrc += c;
					else if (c >= 'a' && c <= 'z') isrc += static_cast<char>(c - 'a' + 'A');
				}
				if (isrc.length() == 12) {
					track.isrc = isrc;
					std::cout << "  Track " << track.trackNumber << ": " << isrc << "\n";
				}
			}
		}
	}

	return true;
}

bool AudioCDCopier::DetectHiddenTrack(DiscInfo& disc) {
	if (disc.tracks.empty() || !disc.tracks[0].isAudio) return false;

	DWORD track1Start = disc.tracks[0].startLBA;
	if (track1Start <= 150) return false;

	std::cout << "\nChecking for hidden track audio...\n";
	m_drive.SetSpeed(4);

	DWORD htStart = 150;
	bool hasAudio = false;

	for (DWORD lba = htStart; lba < track1Start && lba < htStart + 75; lba++) {
		std::vector<BYTE> buf(AUDIO_SECTOR_SIZE);
		if (m_drive.ReadSectorAudioOnly(lba, buf.data())) {
			bool silent = true;
			for (size_t i = 0; i < AUDIO_SECTOR_SIZE && silent; i += 4) {
				int16_t left = *reinterpret_cast<int16_t*>(buf.data() + i);
				int16_t right = *reinterpret_cast<int16_t*>(buf.data() + i + 2);
				if (std::abs(left) > 100 || std::abs(right) > 100) {
					silent = false;
				}
			}
			if (!silent) {
				hasAudio = true;
				break;
			}
		}
	}

	m_drive.SetSpeed(0);

	if (hasAudio) {
		int frames = static_cast<int>(track1Start - 150);
		int seconds = frames / 75;
		Console::SetColor(Console::Color::Yellow);
		std::cout << "  Hidden track detected! Length: " << seconds / 60 << ":"
			<< std::setfill('0') << std::setw(2) << seconds % 60 << "."
			<< std::setw(2) << frames % 75 << std::setfill(' ')
			<< " (" << frames << " frames)\n";
		Console::Reset();

		disc.tracks[0].pregapLBA = 150;
		return true;
	}

	std::cout << "  No hidden track audio found.\n";
	return false;
}