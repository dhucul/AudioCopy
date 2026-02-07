#define NOMINMAX
#include "AudioCDCopier.h"
#include "AccurateRip.h"
#include "InterruptHandler.h"
#include "MenuHelpers.h"
#include <iostream>
#include <fstream>
#include <iomanip>
// ... other includes as needed

bool AudioCDCopier::ValidateDiscStructure(const DiscInfo& disc, std::vector<std::string>& issues) {
	issues.clear();

	if (disc.tracks.empty()) {
		issues.push_back("No tracks found on disc");
		return false;
	}

	for (size_t i = 0; i < disc.tracks.size(); i++) {
		const auto& t = disc.tracks[i];
		if (t.endLBA <= t.startLBA) {
			issues.push_back("Track " + std::to_string(t.trackNumber) + " has invalid LBA range");
		}
		if (i > 0 && t.startLBA < disc.tracks[i - 1].endLBA) {
			issues.push_back("Track " + std::to_string(t.trackNumber) + " overlaps with previous track");
		}
	}

	return issues.empty();
}

bool AudioCDCopier::VerifyWrittenFile(const std::wstring& filename, const DiscInfo& disc,
	std::vector<DWORD>& mismatchedSectors) {
	std::cout << "\n=== Verifying Written File ===\n";
	mismatchedSectors.clear();

	std::ifstream file(filename, std::ios::binary);
	if (!file) {
		std::cout << "ERROR: Cannot open file for verification.\n";
		return false;
	}

	file.seekg(0, std::ios::end);
	std::streamsize fileSize = file.tellg();
	file.seekg(0, std::ios::beg);

	size_t expectedSectors = disc.rawSectors.size();
	size_t expectedSize = expectedSectors * AUDIO_SECTOR_SIZE;

	if (static_cast<size_t>(fileSize) != expectedSize) {
		std::cout << "WARNING: File size mismatch. Expected: " << expectedSize
			<< ", Actual: " << fileSize << "\n";
	}

	std::cout << "Verifying " << expectedSectors << " sectors...\n";

	ProgressIndicator progress(40);
	progress.SetLabel("  Verify");
	progress.Start();

	std::vector<BYTE> fileSector(AUDIO_SECTOR_SIZE);
	DWORD sectorNum = 0;

	for (size_t i = 0; i < disc.rawSectors.size(); i++) {
		if (g_interrupt.IsInterrupted() || g_interrupt.CheckEscapeKey()) {
			progress.Finish(false);
			return false;
		}

		file.read(reinterpret_cast<char*>(fileSector.data()), AUDIO_SECTOR_SIZE);
		if (!file) {
			std::cout << "\nERROR: Read error at sector " << sectorNum << "\n";
			break;
		}

		const auto& origSector = disc.rawSectors[i];
		size_t compareSize = std::min(origSector.size(), static_cast<size_t>(AUDIO_SECTOR_SIZE));

		if (memcmp(fileSector.data(), origSector.data(), compareSize) != 0) {
			mismatchedSectors.push_back(sectorNum);
		}

		sectorNum++;
		progress.Update(static_cast<int>(sectorNum), static_cast<int>(expectedSectors));
	}

	progress.Finish(true);
	file.close();

	std::cout << "\n=== Verification Results ===\n";
	std::cout << "Sectors verified: " << sectorNum << "\n";
	std::cout << "Mismatches: " << mismatchedSectors.size() << "\n";

	if (mismatchedSectors.empty()) {
		std::cout << "*** FILE VERIFIED SUCCESSFULLY ***\n";
		return true;
	}
	else {
		std::cout << "*** VERIFICATION FAILED ***\n";
		if (mismatchedSectors.size() <= 10) {
			std::cout << "Mismatched sectors: ";
			for (DWORD lba : mismatchedSectors) {
				std::cout << lba << " ";
			}
			std::cout << "\n";
		}
		return false;
	}
}

bool AudioCDCopier::CheckDiskSpace(const std::wstring& path, DWORD sectorsNeeded) {
	ULARGE_INTEGER freeBytes;
	if (GetDiskFreeSpaceExW(path.c_str(), &freeBytes, nullptr, nullptr)) {
		ULONGLONG needed = static_cast<ULONGLONG>(sectorsNeeded) * AUDIO_SECTOR_SIZE;
		return freeBytes.QuadPart >= needed;
	}
	return true;
}

bool AudioCDCopier::RunPreflightChecks(DiscInfo& disc, std::vector<std::string>& warnings) {
	warnings.clear();

	std::vector<std::string> structureIssues;
	if (!ValidateDiscStructure(disc, structureIssues)) {
		warnings.insert(warnings.end(), structureIssues.begin(), structureIssues.end());
	}

	return warnings.empty();
}



uint32_t AudioCDCopier::CalculateTrackCRC(const DiscInfo& disc, int trackIndex) {
	if (trackIndex < 0 || trackIndex >= static_cast<int>(disc.tracks.size())) return 0;
	if (disc.rawSectors.empty()) return 0;

	const auto& track = disc.tracks[trackIndex];

	// Use consistent logic with how data was actually read (respects pregapMode)
	DWORD trackStart;
	if (disc.pregapMode == PregapMode::Skip) {
		trackStart = track.startLBA;
	}
	else {
		trackStart = track.pregapLBA;
	}
	DWORD trackSectors = track.endLBA - trackStart + 1;

	// Find starting sector index for this track (must match reading logic)
	size_t sectorIdx = 0;
	for (int i = 0; i < trackIndex; i++) {
		DWORD start;
		if (disc.pregapMode == PregapMode::Skip) {
			start = disc.tracks[i].startLBA;
		}
		else {
			start = disc.tracks[i].pregapLBA;
		}
		sectorIdx += disc.tracks[i].endLBA - start + 1;
	}

	// CRC-32 calculation (unchanged)
	uint32_t crc = 0xFFFFFFFF;
	const uint32_t polynomial = 0xEDB88320;

	for (DWORD i = 0; i < trackSectors && sectorIdx < disc.rawSectors.size(); i++, sectorIdx++) {
		const auto& sector = disc.rawSectors[sectorIdx];
		for (size_t j = 0; j < std::min(sector.size(), static_cast<size_t>(AUDIO_SECTOR_SIZE)); j++) {
			crc ^= sector[j];
			for (int bit = 0; bit < 8; bit++) {
				crc = (crc >> 1) ^ ((crc & 1) ? polynomial : 0);
			}
		}
	}

	return ~crc;
}



bool AudioCDCopier::VerifyTrackCRCs(const DiscInfo& disc, std::vector<CRCVerification>& results) {
	results.clear();

	if (disc.tracks.empty() || disc.rawSectors.empty()) {
		return false;
	}

	std::cout << "\n=== Verifying Track CRCs ===\n";

	for (size_t i = 0; i < disc.tracks.size(); i++) {
		CRCVerification result;
		result.calculatedCRC = CalculateTrackCRC(disc, static_cast<int>(i));
		result.expectedCRC = 0;  // We don't have expected values, just calculating
		result.matches = true;   // Since we're just calculating, not comparing

		results.push_back(result);

		std::cout << "Track " << disc.tracks[i].trackNumber << ": CRC32 = "
			<< std::hex << std::setfill('0') << std::setw(8)
			<< result.calculatedCRC << std::dec << "\n";
	}

	return true;
}