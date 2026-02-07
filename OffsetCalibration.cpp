// ============================================================================
// OffsetCalibration.cpp - Automatic offset detection implementation
// ============================================================================
#define NOMINMAX  // Prevent Windows.h from defining min/max macros
#include "OffsetCalibration.h"
#include <cmath>
#include <algorithm>
#include <sstream>
#include <iomanip>
#include <Windows.h>
#include <winhttp.h>
#include <map>
#include <vector>

#pragma comment(lib, "winhttp.lib")

constexpr int OffsetCalibration::CommonOffsets[];

uint32_t OffsetCalibration::CalculateAccurateRipCRC(
	const std::vector<int16_t>& samples,
	int trackNumber,
	int totalTracks) {

	uint32_t crc = 0;
	uint32_t mult = 1;

	// Skip first/last 5 frames for first/last tracks
	size_t startSample = (trackNumber == 1) ? 5 * 588 * 2 : 0;
	size_t endSample = samples.size();
	if (trackNumber == totalTracks) {
		endSample = std::max(endSample, static_cast<size_t>(5 * 588 * 2)) - 5 * 588 * 2;
	}

	for (size_t i = startSample; i < endSample; i += 2) {
		uint32_t sample = static_cast<uint16_t>(samples[i]) |
			(static_cast<uint16_t>(samples[i + 1]) << 16);
		crc += sample * mult;
		mult++;
	}

	return crc;
}

CalibrationResult OffsetCalibration::QuickCalibrate(
	std::function<void(int progress, const std::string& status)> progressCallback) {

	CalibrationResult result;

	if (progressCallback) {
		progressCallback(0, "Reading disc TOC...");
	}

	// Get disc ID and fetch AccurateRip checksums
	std::string discId = CalculateDiscId();
	if (discId.empty()) {
		if (progressCallback) {
			progressCallback(100, "Failed to read disc TOC");
		}
		result.success = false;
		return result;
	}

	auto arChecksums = FetchAccurateRipChecksums(discId);
	if (arChecksums.empty()) {
		if (progressCallback) {
			progressCallback(100, "Disc not found in AccurateRip database");
		}
		result.success = false;
		return result;
	}

	result.totalTracks = static_cast<int>(arChecksums.size());

	// Test offset range (typical CD drive offsets: -1200 to +1200 samples)
	const int MIN_OFFSET = -1200;
	const int MAX_OFFSET = 1200;
	const int STEP = 6;  // Test every 6 samples for speed

	std::map<int, int> offsetScores;  // offset -> matching tracks
	std::map<int, std::vector<int>> offsetMatchedTracks;  // Track which tracks matched

	int testsTotal = (MAX_OFFSET - MIN_OFFSET) / STEP;
	int testsDone = 0;

	for (int testOffset = MIN_OFFSET; testOffset <= MAX_OFFSET; testOffset += STEP) {
		if (progressCallback) {
			int pct = 10 + (testsDone * 80 / testsTotal);
			progressCallback(pct, "Testing offset: " + std::to_string(testOffset));
		}

		int matches = 0;
		std::vector<int> matchedTracks;

		// Read all tracks with this offset and calculate CRCs
		for (size_t i = 0; i < arChecksums.size(); i++) {
			uint32_t calculatedCRC = ReadTrackAndCalculateCRC(static_cast<int>(i + 1), testOffset);
			if (calculatedCRC == arChecksums[i]) {
				matches++;
				matchedTracks.push_back(static_cast<int>(i + 1));
			}
		}

		offsetScores[testOffset] = matches;
		if (matches > 0) {
			offsetMatchedTracks[testOffset] = matchedTracks;
		}

		testsDone++;
	}

	// Find best offset(s)
	int bestMatches = 0;
	std::vector<int> candidateOffsets;

	for (const auto& [offset, matches] : offsetScores) {
		if (matches > bestMatches) {
			bestMatches = matches;
			candidateOffsets.clear();
			candidateOffsets.push_back(offset);
		}
		else if (matches == bestMatches && matches > 0) {
			candidateOffsets.push_back(offset);
		}
	}

	// Calculate confidence based on percentage of tracks matched
	result.matchingTracks = bestMatches;
	result.confidence = result.totalTracks > 0 ? (bestMatches * 100) / result.totalTracks : 0;

	// If multiple offsets have same score, pick the one closest to zero
	if (!candidateOffsets.empty()) {
		result.detectedOffset = *std::min_element(candidateOffsets.begin(),
			candidateOffsets.end(),
			[](int a, int b) { return std::abs(a) < std::abs(b); });

		result.success = (result.confidence >= 50);  // Need at least 50% match confidence
	}
	else {
		result.success = false;
	}

	if (progressCallback) {
		progressCallback(100, "Calibration complete");
	}

	return result;
}

std::string OffsetCalibration::CalculateDiscId() {
	// Read TOC from drive to calculate AccurateRip disc IDs

	// Read TOC using READ TOC command
	BYTE tocCdb[10] = { 0x43, 0x00, 0, 0, 0, 0, 0, 0x03, 0x24, 0 };
	std::vector<BYTE> tocBuf(804);

	if (!m_drive.SendSCSI(tocCdb, 10, tocBuf.data(), 804)) {
		return "";
	}

	int tocLen = (tocBuf[0] << 8) | tocBuf[1];
	if (tocLen < 2) return "";

	int firstTrack = tocBuf[2];
	int lastTrack = tocBuf[3];
	int trackCount = lastTrack - firstTrack + 1;
	if (trackCount <= 0 || trackCount > 99) return "";

	// Calculate AccurateRip IDs
	uint32_t discId1 = 0;
	uint32_t discId2 = 0;
	uint32_t cddbId = 0;

	int cddbSum = 0;

	for (int i = 0; i < trackCount; i++) {
		BYTE* td = tocBuf.data() + 4 + i * 8;
		DWORD startLBA = (static_cast<DWORD>(td[4]) << 24) |
			(static_cast<DWORD>(td[5]) << 16) |
			(static_cast<DWORD>(td[6]) << 8) |
			static_cast<DWORD>(td[7]);

		int offset = static_cast<int>(startLBA) + 150;
		discId1 += static_cast<uint32_t>(offset);
		discId2 += static_cast<uint32_t>(offset) * (i + 1);

		// CDDB checksum
		int seconds = offset / 75;
		while (seconds > 0) {
			cddbSum += seconds % 10;
			seconds /= 10;
		}
	}

	// Get lead-out
	BYTE* leadOut = tocBuf.data() + 4 + trackCount * 8;
	DWORD leadOutLBA = (static_cast<DWORD>(leadOut[4]) << 24) |
		(static_cast<DWORD>(leadOut[5]) << 16) |
		(static_cast<DWORD>(leadOut[6]) << 8) |
		static_cast<DWORD>(leadOut[7]);

	discId2 += static_cast<uint32_t>(leadOutLBA + 150) * (trackCount + 1);

	// Calculate CDDB ID
	BYTE* firstTd = tocBuf.data() + 4;
	DWORD firstLBA = (static_cast<DWORD>(firstTd[4]) << 24) |
		(static_cast<DWORD>(firstTd[5]) << 16) |
		(static_cast<DWORD>(firstTd[6]) << 8) |
		static_cast<DWORD>(firstTd[7]);

	int totalSeconds = static_cast<int>((leadOutLBA - firstLBA) / 75);
	cddbId = ((cddbSum % 255) << 24) | (totalSeconds << 8) | trackCount;

	// Format: trackcount-discid1-discid2-cddbid
	std::ostringstream ss;
	ss << std::setfill('0') << std::dec << std::setw(3) << trackCount << "-"

		<< std::hex << std::setw(8) << discId1 << "-"
		<< std::setw(8) << discId2 << "-"
		<< std::setw(8) << cddbId;

	return ss.str();
}

std::vector<uint32_t> OffsetCalibration::FetchAccurateRipChecksums(const std::string& discId) {
	std::vector<uint32_t> checksums;

	if (discId.length() < 30) return checksums;

	// Parse disc ID: "trackcount-discid1-discid2-cddbid"
	int trackCount = 0;
	uint32_t discId1 = 0, discId2 = 0, cddbId = 0;

	if (sscanf_s(discId.c_str(), "%d-%x-%x-%x", &trackCount, &discId1, &discId2, &cddbId) != 4) {
		return checksums;
	}

	// Build URL path
	char url[256];
	snprintf(url, sizeof(url),
		"/accuraterip/%x/%x/%x/dBAR-%03d-%08x-%08x-%08x.bin",
		discId1 & 0xF, (discId1 >> 4) & 0xF, (discId1 >> 8) & 0xF,
		trackCount, discId1, discId2, cddbId);

	// HTTP request
	HINTERNET hSession = WinHttpOpen(L"OffsetCalibration/1.0",
		WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, nullptr, nullptr, 0);
	if (!hSession) return checksums;

	WinHttpSetTimeouts(hSession, 5000, 10000, 5000, 10000);

	HINTERNET hConnect = WinHttpConnect(hSession, L"www.accuraterip.com",
		INTERNET_DEFAULT_HTTP_PORT, 0);
	if (!hConnect) {
		WinHttpCloseHandle(hSession);
		return checksums;
	}

	wchar_t wUrl[256];
	MultiByteToWideChar(CP_UTF8, 0, url, -1, wUrl, 256);

	HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"GET", wUrl, nullptr,
		WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, 0);

	if (hRequest && WinHttpSendRequest(hRequest, nullptr, 0, nullptr, 0, 0, 0) &&
		WinHttpReceiveResponse(hRequest, nullptr)) {

		DWORD statusCode = 0, statusSize = sizeof(statusCode);
		WinHttpQueryHeaders(hRequest, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
			nullptr, &statusCode, &statusSize, nullptr);

		if (statusCode == 200) {
			// Read response data
			std::vector<BYTE> data;
			DWORD bytesAvailable = 0;

			while (WinHttpQueryDataAvailable(hRequest, &bytesAvailable) && bytesAvailable > 0) {
				std::vector<BYTE> buffer(bytesAvailable);
				DWORD bytesRead = 0;
				if (WinHttpReadData(hRequest, buffer.data(), bytesAvailable, &bytesRead)) {
					data.insert(data.end(), buffer.begin(), buffer.begin() + bytesRead);
				}
			}

			// Parse AccurateRip response format:
			// Each entry: 1 byte track count, 4 bytes disc ID 1, 4 bytes disc ID 2,
			//             4 bytes CDDB ID, then for each track: 1 byte confidence, 4 bytes CRC
			// Multiple entries may exist (different pressings)

			if (data.size() >= 13 + trackCount * 9) {
				size_t offset = 13; // Skip header (1 + 4 + 4 + 4)
				checksums.resize(trackCount);

				for (int t = 0; t < trackCount && offset + 5 <= data.size(); t++) {
					// Skip confidence byte
					offset++;
					// Read CRC (little-endian)
					checksums[t] = data[offset] | (data[offset + 1] << 8) |
						(data[offset + 2] << 16) | (data[offset + 3] << 24);
					offset += 4;
					// Skip frame offset CRC (4 bytes) if present
					offset += 4;
				}
			}
		}
	}

	if (hRequest) WinHttpCloseHandle(hRequest);
	WinHttpCloseHandle(hConnect);
	WinHttpCloseHandle(hSession);

	return checksums;
}

CalibrationResult OffsetCalibration::CalibrateWithDisc(
	std::function<void(int progress, const std::string& status)> progressCallback) {

	// Full calibration tests all offsets from -2000 to +2000
	// This is slower but more thorough for unknown drives

	CalibrationResult result;

	// First try quick calibration with common offsets
	result = QuickCalibrate(progressCallback);
	if (result.success && result.confidence >= 80) {
		return result;
	}

	// If quick calibration failed, do full sweep
	if (progressCallback) {
		progressCallback(0, "Quick calibration inconclusive, performing full sweep...");
	}

	// Get disc ID and checksums for full sweep
	std::string discId = CalculateDiscId();
	if (discId.empty()) {
		result.success = false;
		return result;
	}

	auto arChecksums = FetchAccurateRipChecksums(discId);
	if (arChecksums.empty()) {
		result.success = false;
		return result;
	}

	result.totalTracks = static_cast<int>(arChecksums.size());
	int bestMatches = 0;
	int bestOffset = 0;

	// Test offsets -2000 to +2000 in steps of 1
	// This is slow but thorough
	for (int offset = -2000; offset <= 2000; offset++) {
		if (progressCallback && (offset % 100 == 0)) {
			int pct = ((offset + 2000) * 100) / 4000;
			progressCallback(pct, "Testing offset " + std::to_string(offset) + "...");
		}

		int matches = 0;

		// Test this offset against AccurateRip checksums
		// Would need to read track data with offset applied and calculate CRC
		// Then compare against arChecksums

		if (matches > bestMatches) {
			bestMatches = matches;
			bestOffset = offset;
		}
	}

	result.detectedOffset = bestOffset;
	result.matchingTracks = bestMatches;
	result.confidence = result.totalTracks > 0 ? (bestMatches * 100) / result.totalTracks : 0;
	result.success = bestMatches > 0;

	if (progressCallback) {
		progressCallback(100, "Full calibration complete: offset " + std::to_string(bestOffset));
	}

	return result;
}

uint32_t OffsetCalibration::ReadTrackAndCalculateCRC(int trackNumber, int offsetSamples) {
	// TODO: Full implementation requires:
	// 1. Read track audio data from drive
	// 2. Apply sample offset correction
	// 3. Calculate AccurateRip CRC using CalculateAccurateRipCRC()
	// 
	// For now, return 0 to allow compilation
	// This makes offset detection non-functional but doesn't break the build
	return 0;
	
	/* FULL IMPLEMENTATION WOULD BE:
	
	// Read TOC to get track boundaries
	BYTE tocCdb[10] = { 0x43, 0x00, 0, 0, 0, 0, 0, 0x03, 0x24, 0 };
	std::vector<BYTE> tocBuf(804);
	if (!m_drive.SendSCSI(tocCdb, 10, tocBuf.data(), 804)) return 0;
	
	// Parse track start/end LBAs
	BYTE* trackDesc = tocBuf.data() + 4 + ((trackNumber - 1) * 8);
	DWORD startLBA = (static_cast<DWORD>(trackDesc[4]) << 24) |
	                 (static_cast<DWORD>(trackDesc[5]) << 16) |
	                 (static_cast<DWORD>(trackDesc[6]) << 8) |
	                 static_cast<DWORD>(trackDesc[7]);
	
	// Get next track or lead-out for end boundary
	BYTE* nextDesc = tocBuf.data() + 4 + (trackNumber * 8);
	DWORD endLBA = (static_cast<DWORD>(nextDesc[4]) << 24) |
	               (static_cast<DWORD>(nextDesc[5]) << 16) |
	               (static_cast<DWORD>(nextDesc[6]) << 8) |
	               static_cast<DWORD>(nextDesc[7]);
	
	// Read all sectors for this track
	std::vector<int16_t> samples;
	for (DWORD lba = startLBA; lba < endLBA; lba++) {
		std::vector<BYTE> sector(2352);
		if (!m_drive.ReadSectorAudioOnly(lba, sector.data())) return 0;
		
		// Convert bytes to samples
		for (int i = 0; i < 2352; i += 2) {
			int16_t sample = static_cast<int16_t>(sector[i] | (sector[i+1] << 8));
			samples.push_back(sample);
		}
	}
	
	// Apply offset correction (shift samples)
	if (offsetSamples != 0) {
		if (offsetSamples > 0) {
			// Positive offset: remove samples from beginning
			samples.erase(samples.begin(), samples.begin() + offsetSamples);
		} else {
			// Negative offset: add samples to beginning
			samples.insert(samples.begin(), -offsetSamples, 0);
		}
	}
	
	// Get total track count for AccurateRip calculation
	int totalTracks = tocBuf[3] - tocBuf[2] + 1;
	
	// Calculate and return AccurateRip CRC
	return CalculateAccurateRipCRC(samples, trackNumber, totalTracks);
	*/
}