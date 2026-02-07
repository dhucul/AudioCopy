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
// File Output
// ============================================================================

bool AudioCDCopier::SaveToFile(const DiscInfo& disc, const std::wstring& base) {
	// Calculate and display original disc IDs for verification
	uint32_t originalDiscID1 = AccurateRip::CalculateDiscID1(disc);
	uint32_t originalDiscID2 = AccurateRip::CalculateDiscID2(disc);
	uint32_t originalCDDB = AccurateRip::CalculateCDDBID(disc);

	std::cout << "\n=== IMPORTANT: Original Disc AccurateRip IDs ===\n";
	std::cout << "These IDs are from the ORIGINAL disc TOC.\n";
	std::cout << "Burned copies will have DIFFERENT IDs but identical audio.\n";
	std::cout << "  Disc ID 1: " << std::hex << std::setfill('0')
		<< std::setw(8) << originalDiscID1 << std::dec << "\n";
	std::cout << "  Disc ID 2: " << std::hex << std::setfill('0')
		<< std::setw(8) << originalDiscID2 << std::dec << "\n";
	std::cout << "  CDDB ID:   " << std::hex << std::setfill('0')
		<< std::setw(8) << originalCDDB << std::dec << "\n";
	std::cout << "These IDs are saved in the .cue file for reference.\n\n";

	std::ofstream img(base + L".bin", std::ios::binary);
	if (!img) return false;

	std::ofstream sub;
	if (disc.includeSubchannel) {
		sub.open(base + L".sub", std::ios::binary);
		if (!sub) return false;
	}

	std::vector<std::wstring> pregapFiles;

	size_t sectorIdx = 0;
	for (size_t i = 0; i < disc.tracks.size(); i++) {
		const auto& t = disc.tracks[i];
		if (disc.selectedSession > 0 && t.session != disc.selectedSession) continue;

		DWORD start = t.pregapLBA;
		DWORD count = t.endLBA - start + 1;

		if (disc.pregapMode == PregapMode::Skip) {
			start = t.startLBA;
			count = t.endLBA - start + 1;
		}
		else if (disc.pregapMode == PregapMode::Separate && t.pregapLBA < t.startLBA) {
			std::wstring pregapPath = base + L"_track" +
				std::to_wstring(t.trackNumber) + L"_pregap.bin";
			std::ofstream pregapFile(pregapPath, std::ios::binary);
			if (pregapFile) {
				DWORD pregapCount = t.startLBA - t.pregapLBA;
				for (DWORD j = 0; j < pregapCount && sectorIdx < disc.rawSectors.size(); j++) {
					const auto& s = disc.rawSectors[sectorIdx++];
					pregapFile.write(reinterpret_cast<const char*>(s.data()), AUDIO_SECTOR_SIZE);
				}
				pregapFiles.push_back(pregapPath);
			}
			start = t.startLBA;
			count = t.endLBA - t.startLBA + 1;
		}

		for (DWORD j = 0; j < count && sectorIdx < disc.rawSectors.size(); j++) {
			const auto& s = disc.rawSectors[sectorIdx++];
			img.write(reinterpret_cast<const char*>(s.data()), AUDIO_SECTOR_SIZE);

			if (disc.includeSubchannel && t.isAudio && s.size() > AUDIO_SECTOR_SIZE) {
				sub.write(reinterpret_cast<const char*>(s.data() + AUDIO_SECTOR_SIZE), SUBCHANNEL_SIZE);
			}
		}
	}

	std::string fn(base.begin(), base.end());
	size_t p = fn.find_last_of("/\\");
	if (p != std::string::npos) fn = fn.substr(p + 1);

	std::ofstream cue(base + L".cue");

	if (!disc.cdText.albumArtist.empty()) {
		cue << "PERFORMER \"" << disc.cdText.albumArtist << "\"\n";
	}
	if (!disc.cdText.albumTitle.empty()) {
		cue << "TITLE \"" << disc.cdText.albumTitle << "\"\n";
	}

	if (disc.includeSubchannel) cue << "REM Subchannel in " << fn << ".sub\n";

	// AccurateRip disc identification
	cue << "REM DISCID " << std::hex << std::setfill('0')
		<< std::setw(8) << AccurateRip::CalculateCDDBID(disc) << std::dec << "\n";
	cue << "REM ACCURATERIPID " << std::hex << std::setfill('0')
		<< std::setw(8) << AccurateRip::CalculateDiscID1(disc) << "-"
		<< std::setw(8) << AccurateRip::CalculateDiscID2(disc) << std::dec << "\n";

	cue << "FILE \"" << fn << ".bin\" BINARY\n";

	DWORD off = 0;
	for (size_t i = 0; i < disc.tracks.size(); i++) {
		const auto& t = disc.tracks[i];
		cue << "  TRACK " << std::setfill('0') << std::setw(2) << t.trackNumber;
		cue << (t.isAudio ? " AUDIO\n" : " MODE1/2352\n");

		if (t.trackNumber > 0 &&
			static_cast<size_t>(t.trackNumber) <= disc.cdText.trackTitles.size() &&
			!disc.cdText.trackTitles[t.trackNumber - 1].empty()) {
			cue << "    TITLE \"" << disc.cdText.trackTitles[t.trackNumber - 1] << "\"\n";
		}
		if (t.trackNumber > 0 &&
			static_cast<size_t>(t.trackNumber) <= disc.cdText.trackArtists.size() &&
			!disc.cdText.trackArtists[t.trackNumber - 1].empty()) {
			cue << "    PERFORMER \"" << disc.cdText.trackArtists[t.trackNumber - 1] << "\"\n";
		}

		if (!t.isrc.empty()) {
			cue << "    ISRC " << t.isrc << "\n";
		}

		DWORD gap = t.startLBA - t.pregapLBA;

		if (disc.pregapMode == PregapMode::Include) {
			if (gap > 0) {
				cue << "    INDEX 00 " << std::setfill('0') << std::setw(2) << off / 75 / 60 << ":"
					<< std::setw(2) << (off / 75) % 60 << ":" << std::setw(2) << off % 75 << "\n";
				off += gap;
			}
			cue << "    INDEX 01 " << std::setfill('0') << std::setw(2) << off / 75 / 60 << ":"
				<< std::setw(2) << (off / 75) % 60 << ":" << std::setw(2) << off % 75 << "\n";
			off += t.endLBA - t.startLBA + 1;
		}
		else {
			if (gap > 0 && i > 0) {
				cue << "    PREGAP " << std::setfill('0') << std::setw(2) << gap / 75 / 60 << ":"
					<< std::setw(2) << (gap / 75) % 60 << ":" << std::setw(2) << gap % 75 << "\n";
			}
			cue << "    INDEX 01 " << std::setfill('0') << std::setw(2) << off / 75 / 60 << ":"
				<< std::setw(2) << (off / 75) % 60 << ":" << std::setw(2) << off % 75 << "\n";
			off += t.endLBA - t.startLBA + 1;
		}
	}

	std::cout << "\n=== Files Created ===\n";
	std::wcout << L"  " << base << L".bin\n";
	if (disc.includeSubchannel) std::wcout << L"  " << base << L".sub\n";
	std::wcout << L"  " << base << L".cue\n";

	for (const auto& pf : pregapFiles) {
		std::wcout << L"  " << pf << L"\n";
	}

	return true;
}

bool AudioCDCopier::SaveBlerLog(const BlerResult& result, const std::wstring& filename) {
	std::ofstream log(filename);
	if (!log) return false;

	log << "# BLER Quality Scan Log\n";
	log << "# Summary:\n";
	log << "#   Total Sectors: " << result.totalSectors << "\n";
	log << "#   Total C2 Errors: " << result.totalC2Errors << " bits\n";
	log << "#   Quality Rating: " << result.qualityRating << "\n";
	log << "#\n";
	log << "Second,LBA,C2_Errors\n";

	DWORD firstLBA = 0;
	for (const auto& t : result.perSecondC2) {
		if (t.first != 0 || t.second != 0) {
			firstLBA = static_cast<DWORD>(t.first);
			break;
		}
	}

	for (size_t i = 0; i < result.perSecondC2.size(); i++) {
		DWORD lba = firstLBA + static_cast<DWORD>(i * 75);
		int c2 = result.perSecondC2[i].second;

		int minutes = static_cast<int>(i) / 60;
		int seconds = static_cast<int>(i) % 60;

		log << minutes << ":" << std::setfill('0') << std::setw(2) << seconds
			<< "," << lba << "," << c2 << "\n";
	}

	log.close();
	return true;
}

bool AudioCDCopier::SaveReadLog(const DiscInfo& disc, const std::wstring& filename) {
	if (disc.readLog.empty()) {
		return false;
	}

	std::ofstream log(filename);
	if (!log) {
		return false;
	}

	log << "# Sector Read Log\n";
	log << "# Format: LBA,Track,ReadTime(ms)\n";
	log << "LBA,Track,ReadTimeMs\n";

	for (const auto& entry : disc.readLog) {
		log << std::get<0>(entry) << ","
			<< std::get<1>(entry) << ","
			<< std::fixed << std::setprecision(2) << std::get<2>(entry) << "\n";
	}

	log.close();
	return true;
}

bool AudioCDCopier::GenerateCueSheet(const DiscInfo& disc, const std::wstring& audioFilePath,
	const std::wstring& cueOutputPath) {
	std::ofstream cue(cueOutputPath);
	if (!cue) return false;

	std::string fn(audioFilePath.begin(), audioFilePath.end());
	size_t p = fn.find_last_of("/\\");
	if (p != std::string::npos) fn = fn.substr(p + 1);

	if (!disc.cdText.albumArtist.empty()) {
		cue << "PERFORMER \"" << disc.cdText.albumArtist << "\"\n";
	}
	if (!disc.cdText.albumTitle.empty()) {
		cue << "TITLE \"" << disc.cdText.albumTitle << "\"\n";
	}

	cue << "FILE \"" << fn << "\" WAVE\n";

	DWORD off = 0;
	for (size_t i = 0; i < disc.tracks.size(); i++) {
		const auto& t = disc.tracks[i];
		cue << "  TRACK " << std::setfill('0') << std::setw(2) << t.trackNumber;
		cue << (t.isAudio ? " AUDIO\n" : " MODE1/2352\n");

		if (t.trackNumber > 0 &&
			static_cast<size_t>(t.trackNumber) <= disc.cdText.trackTitles.size() &&
			!disc.cdText.trackTitles[t.trackNumber - 1].empty()) {
			cue << "    TITLE \"" << disc.cdText.trackTitles[t.trackNumber - 1] << "\"\n";
		}
		if (t.trackNumber > 0 &&
			static_cast<size_t>(t.trackNumber) <= disc.cdText.trackArtists.size() &&
			!disc.cdText.trackArtists[t.trackNumber - 1].empty()) {
			cue << "    PERFORMER \"" << disc.cdText.trackArtists[t.trackNumber - 1] << "\"\n";
		}

		if (!t.isrc.empty()) {
			cue << "    ISRC " << t.isrc << "\n";
		}

		DWORD gap = t.startLBA - t.pregapLBA;

		if (disc.pregapMode == PregapMode::Include) {
			if (gap > 0) {
				cue << "    INDEX 00 " << std::setfill('0') << std::setw(2) << off / 75 / 60 << ":"
					<< std::setw(2) << (off / 75) % 60 << ":" << std::setw(2) << off % 75 << "\n";
				off += gap;
			}
			cue << "    INDEX 01 " << std::setfill('0') << std::setw(2) << off / 75 / 60 << ":"
				<< std::setw(2) << (off / 75) % 60 << ":" << std::setw(2) << off % 75 << "\n";
			off += t.endLBA - t.startLBA + 1;
		}
		else {
			if (gap > 0 && i > 0) {
				cue << "    PREGAP " << std::setfill('0') << std::setw(2) << gap / 75 / 60 << ":"
					<< std::setw(2) << (gap / 75) % 60 << ":" << std::setw(2) << gap % 75 << "\n";
			}
			cue << "    INDEX 01 " << std::setfill('0') << std::setw(2) << off / 75 / 60 << ":"
				<< std::setw(2) << (off / 75) % 60 << ":" << std::setw(2) << off % 75 << "\n";
			off += t.endLBA - t.startLBA + 1;
		}
	}

	return true;
}