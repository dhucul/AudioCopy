#define NOMINMAX
#include "AudioCDCopier.h"
#include "AccurateRip.h"
#include "InterruptHandler.h"
#include "MenuHelpers.h"
#include <ntddcdrm.h>
#include <iostream>
#include <fstream>
#include <iomanip>
#include <chrono>
#include <algorithm>
#include <map>          // ✅ ADD THIS
#include <cmath>
#include <sstream>
#include <ctime>
#include <conio.h>
#include <vector>
#include <cstring>      // ✅ ADD THIS (for memcpy)

// ============================================================================
// Disc Reading
// ============================================================================

bool AudioCDCopier::ReadSectorWithRetry(DWORD lba, BYTE* data, int sectorSize,
	bool isAudio, bool includeSubchannel, int& retryCount, bool detectC2, int* c2Errors) {
	if (c2Errors) *c2Errors = 0;

	for (int attempt = 0; attempt < MAX_RETRIES; attempt++) {
		bool ok = false;
		int sectorC2 = 0;

		if (isAudio) {
			if (detectC2) {
				ok = m_drive.ReadSectorWithC2(lba, data,
					includeSubchannel ? data + AUDIO_SECTOR_SIZE : nullptr, sectorC2);
				if (ok && c2Errors) *c2Errors = sectorC2;
			}
			else if (includeSubchannel) {
				ok = m_drive.ReadSector(lba, data, data + AUDIO_SECTOR_SIZE);
			}
			else {
				ok = m_drive.ReadSectorAudioOnly(lba, data);
			}
		}
		else {
			ok = m_drive.ReadDataSector(lba, data);
		}

		if (ok) {
			retryCount += attempt;
			return true;
		}

		if (attempt == 0) m_drive.SetSpeed(RETRY_SPEED_REDUCTION);
		Sleep(50 * (attempt + 1));
	}
	return false;
}

bool AudioCDCopier::ReadDisc(DiscInfo& disc, int errorMode, std::function<void(int, int)> progress) {
	DWORD total = 0;
	for (size_t i = 0; i < disc.tracks.size(); i++) {
		if (disc.selectedSession > 0 && disc.tracks[i].session != disc.selectedSession) continue;

		DWORD start;
		if (disc.pregapMode == PregapMode::Skip) {
			start = disc.tracks[i].startLBA;
		}
		else {
			start = disc.tracks[i].pregapLBA;
		}

		DWORD count = disc.tracks[i].endLBA - start + 1;
		if (total > UINT32_MAX - count) {
			std::cerr << "Error: Disc too large\n";
			return false;
		}
		total += count;
	}

	try {
		disc.rawSectors.clear();
		disc.rawSectors.reserve(total);
	}
	catch (const std::bad_alloc&) {
		std::cerr << "Error: Not enough memory for " << total << " sectors\n";
		return false;
	}

	disc.errorCount = 0;
	disc.badSectors.clear();
	disc.readLog.clear();
	if (disc.loggingOutput != LogOutput::None) disc.readLog.reserve(total);

	std::cout << "  (Press ESC or Ctrl+C to cancel)\n" << std::flush;

	if (progress) progress(0, total);

	DWORD cur = 0;
	int totalRetries = 0;

	for (size_t i = 0; i < disc.tracks.size(); i++) {
		auto& t = disc.tracks[i];
		if (disc.selectedSession > 0 && t.session != disc.selectedSession) continue;

		DWORD start;
		if (disc.pregapMode == PregapMode::Skip) {
			start = t.startLBA;
		}
		else {
			start = t.pregapLBA;
		}

		int sectorSize = (disc.includeSubchannel && t.isAudio) ? RAW_SECTOR_SIZE : AUDIO_SECTOR_SIZE;

		for (DWORD lba = start; lba <= t.endLBA; lba++) {
			if (g_interrupt.IsInterrupted() || g_interrupt.CheckEscapeKey()) {
				g_interrupt.SetInterrupted(true);
				std::cout << "\n\n*** Read cancelled ***\n";
				_getch();
				return false;
			}

			if (disc.enableCacheDefeat && cur > 0) {
				DefeatDriveCache(lba, disc.leadOutLBA);  // Add disc.leadOutLBA
			}

			std::vector<BYTE> sec(sectorSize, 0);
			int retries = 0, c2Errors = 0;
			auto sectorStart = std::chrono::steady_clock::now();
			bool ok = ReadSectorWithRetry(lba, sec.data(), sectorSize, t.isAudio,
				disc.includeSubchannel, retries, disc.enableC2Detection && t.isAudio, &c2Errors);
			double sectorTime = std::chrono::duration<double, std::milli>(
				std::chrono::steady_clock::now() - sectorStart).count();

			totalRetries += retries;
			if (c2Errors > 0) {
				disc.c2ErrorSectors.push_back(lba);
				disc.totalC2Errors += c2Errors;
			}

			if (!ok) {
				disc.errorCount++;
				disc.badSectors.push_back(lba);
				std::cerr << "\n  Read error at LBA " << lba << " (Track " << t.trackNumber << ")";
				if (errorMode == 1) { std::cerr << " - Aborting\n"; return false; }
				else if (errorMode == 2) std::cerr << " - Filled with silence\n";
				else { std::cerr << " - Skipped\n"; if (progress) progress(++cur, total); continue; }
			}

			bool logToFile = (disc.loggingOutput == LogOutput::File || disc.loggingOutput == LogOutput::Both);

			if (logToFile) {
				disc.readLog.emplace_back(lba, t.trackNumber, sectorTime);
			}

			disc.rawSectors.push_back(std::move(sec));
			if (progress) progress(++cur, total);
		}
	}

	if (totalRetries > 0 || disc.errorCount > 0) {
		std::cout << "\n  Retries: " << totalRetries << ", Unrecoverable: " << disc.errorCount << "\n";
	}

	return true;
}

bool AudioCDCopier::ReadDiscSecure(DiscInfo& disc, const SecureRipConfig& config,
	SecureRipResult& result, std::function<void(int, int)> progress) {

	SecureRipConfig effectiveConfig = config;
	effectiveConfig.cacheDefeat = disc.enableCacheDefeat;

	DWORD total = 0;
	for (size_t i = 0; i < disc.tracks.size(); i++) {
		if (disc.selectedSession > 0 && disc.tracks[i].session != disc.selectedSession) continue;
		DWORD start = (disc.pregapMode == PregapMode::Skip) ? disc.tracks[i].startLBA : disc.tracks[i].pregapLBA;
		total += disc.tracks[i].endLBA - start + 1;
	}

	result = SecureRipResult{};
	result.totalSectors = static_cast<int>(total);

	try {
		disc.rawSectors.clear();
		disc.rawSectors.reserve(total);
	}
	catch (const std::bad_alloc&) {
		std::cerr << "Error: Not enough memory\n";
		return false;
	}

	disc.errorCount = 0;
	disc.badSectors.clear();
	std::cout << "  Secure rip mode: " << effectiveConfig.minPasses << "-" << effectiveConfig.maxPasses
		<< " passes, require " << effectiveConfig.requiredMatches << " matches\n";
	std::cout << "  Cache defeat: " << (effectiveConfig.cacheDefeat ? "ENABLED" : "DISABLED") << "\n";
	std::cout << "  (Press ESC or Ctrl+C to cancel)\n" << std::flush;

	if (progress) progress(0, total);

	DWORD cur = 0;
	int totalPasses = 0;

	for (size_t i = 0; i < disc.tracks.size(); i++) {
		auto& t = disc.tracks[i];
		if (disc.selectedSession > 0 && t.session != disc.selectedSession) continue;
		DWORD start = (disc.pregapMode == PregapMode::Skip) ? t.startLBA : t.pregapLBA;
		int sectorSize = (disc.includeSubchannel && t.isAudio) ? RAW_SECTOR_SIZE : AUDIO_SECTOR_SIZE;

		for (DWORD lba = start; lba <= t.endLBA; lba++) {
			if (g_interrupt.IsInterrupted() || g_interrupt.CheckEscapeKey()) {
				return false;
			}

			std::vector<BYTE> sec(sectorSize, 0);
			SecureSectorResult secResult;

			bool ok = ReadSectorSecure(lba, sec.data(), sectorSize, t.isAudio, effectiveConfig, secResult);
			totalPasses += secResult.totalPasses;

			if (secResult.isSecure) {
				result.secureSectors++;
				if (secResult.passesRequired == 1) result.singlePassSectors++;
				else result.multiPassSectors++;
			}
			else {
				result.unsecureSectors++;
				result.problemSectors.push_back(secResult);
			}

			if (!ok) {
				disc.errorCount++;
				disc.badSectors.push_back(lba);
			}

			disc.rawSectors.push_back(std::move(sec));
			cur++;
			if (progress) progress(cur, total);
		}
	}

	result.averagePassesRequired = total > 0 ? totalPasses / static_cast<int>(total) : 0;
	result.securityConfidence = total > 0 ? static_cast<double>(result.secureSectors) / total * 100.0 : 0;
	result.qualityAssessment = result.securityConfidence >= 99.9 ? "Excellent" :
		result.securityConfidence >= 99.0 ? "Good" :
		result.securityConfidence >= 95.0 ? "Acceptable" : "Poor";

	std::cout << "\n  Secure: " << result.secureSectors << "/" << result.totalSectors
		<< " (" << std::fixed << std::setprecision(1) << result.securityConfidence << "%)\n";

	return true;
}

bool AudioCDCopier::ReadDiscBurst(DiscInfo& disc, std::function<void(int, int)> progress) {
	DWORD total = 0;
	for (size_t i = 0; i < disc.tracks.size(); i++) {
		if (disc.selectedSession > 0 && disc.tracks[i].session != disc.selectedSession) continue;
		DWORD start = (disc.pregapMode == PregapMode::Skip) ? disc.tracks[i].startLBA : disc.tracks[i].pregapLBA;
		total += disc.tracks[i].endLBA - start + 1;
	}

	try {
		disc.rawSectors.clear();
		disc.rawSectors.reserve(total);
	}
	catch (const std::bad_alloc&) {
		std::cerr << "Error: Not enough memory\n";
		return false;
	}

	m_drive.SetSpeed(0);
	std::cout << "  BURST MODE - Maximum speed, no verification\n";
	if (disc.enableCacheDefeat) {
		std::cout << "  Cache defeat: ENABLED (will reduce speed)\n";
	}
	std::cout << "  (Press ESC or Ctrl+C to cancel)\n";

	DWORD cur = 0;
	for (size_t i = 0; i < disc.tracks.size(); i++) {
		auto& t = disc.tracks[i];
		if (disc.selectedSession > 0 && t.session != disc.selectedSession) continue;
		DWORD start = (disc.pregapMode == PregapMode::Skip) ? t.startLBA : t.pregapLBA;
		int sectorSize = (disc.includeSubchannel && t.isAudio) ? RAW_SECTOR_SIZE : AUDIO_SECTOR_SIZE;

		for (DWORD lba = start; lba <= t.endLBA; lba++) {
			if (g_interrupt.IsInterrupted() || g_interrupt.CheckEscapeKey()) {
				return false;
			}

			if (disc.enableCacheDefeat && cur > 0) {
				DefeatDriveCache(lba, disc.leadOutLBA);  // Add disc.leadOutLBA
			}

			std::vector<BYTE> sec(sectorSize, 0);
			bool ok = false;

			if (t.isAudio) {
				if (disc.includeSubchannel) {
					ok = m_drive.ReadSector(lba, sec.data(), sec.data() + AUDIO_SECTOR_SIZE);
				}
				else {
					ok = m_drive.ReadSectorAudioOnly(lba, sec.data());
				}
			}
			else {
				ok = m_drive.ReadDataSector(lba, sec.data());
			}

			if (!ok) {
				disc.errorCount++;
				disc.badSectors.push_back(lba);
			}

			disc.rawSectors.push_back(std::move(sec));
			cur++;
			if (progress) progress(cur, total);
		}
	}

	return true;
}

bool AudioCDCopier::ReadSectorSecure(DWORD lba, BYTE* data, int sectorSize, bool isAudio,
	const SecureRipConfig& config, SecureSectorResult& result) {
	result.lba = lba;
	result.totalPasses = 0;
	result.matchingPasses = 0;
	result.c2ErrorPasses = 0;
	result.isSecure = false;

	std::map<uint32_t, std::pair<int, std::vector<BYTE>>> hashCounts;

	for (int pass = 0; pass < config.maxPasses; pass++) {
		result.totalPasses++;

		if (config.cacheDefeat && pass > 0) {
			DefeatDriveCache(lba, 0);
		}

		// Around line 873 - modify buffer allocation
		std::vector<BYTE> buf(sectorSize);  // Already correct size from line 819
		int c2Errors = 0;
		bool ok = false;

		if (isAudio && config.useC2) {
			ScsiDrive::C2ReadOptions c2Opts;
			c2Opts.countBytes = true;

			// FIX: Pass subchannel pointer when includeSubchannel is true
			BYTE* subchannelPtr = (sectorSize > AUDIO_SECTOR_SIZE) ? buf.data() + AUDIO_SECTOR_SIZE : nullptr;
			ok = m_drive.ReadSectorWithC2Ex(lba, buf.data(), subchannelPtr, c2Errors, nullptr, c2Opts);
			if (c2Errors > 0) result.c2ErrorPasses++;
		}
		else if (isAudio) {
			// FIX: Need subchannel-aware read
			if (sectorSize > AUDIO_SECTOR_SIZE) {
				// Read with subchannel
				ok = m_drive.ReadSector(lba, buf.data(), buf.data() + AUDIO_SECTOR_SIZE);
			}
			else {
				ok = m_drive.ReadSectorAudioOnly(lba, buf.data());
			}
		}

		if (!ok) continue;

		uint32_t hash = HashSector(buf.data(), AUDIO_SECTOR_SIZE);

		if (hashCounts.find(hash) == hashCounts.end()) {
			hashCounts[hash] = { 1, buf };
		}
		else {
			hashCounts[hash].first++;
		}

		// Check if ANY hash has enough matches - single unified check
		for (const auto& hc : hashCounts) {
			if (hc.second.first >= config.requiredMatches &&
				(pass + 1 >= config.minPasses || hc.second.first > config.minPasses)) {
				memcpy(data, hc.second.second.data(), sectorSize);
				result.finalHash = hc.first;
				result.matchingPasses = hc.second.first;
				result.passesRequired = pass + 1;
				result.isSecure = true;
				return true;
			}
		}
		// REMOVED: Redundant second check for hashCounts.size() == 1
	}

	// Fallback: use best available result
	int bestCount = 0;
	uint32_t bestHash = 0;
	for (const auto& hc : hashCounts) {
		if (hc.second.first > bestCount) {
			bestCount = hc.second.first;
			bestHash = hc.first;
		}
	}

	if (bestCount > 0) {
		memcpy(data, hashCounts[bestHash].second.data(), sectorSize);
		result.finalHash = bestHash;
		result.matchingPasses = bestCount;
		result.passesRequired = result.totalPasses;
		result.isSecure = (bestCount >= config.requiredMatches);
	}

	return bestCount > 0;
}
// Keep the old single-parameter version for backward compatibility
bool AudioCDCopier::DefeatDriveCache(DWORD currentLBA, DWORD maxLBA) {
	// Most drives have 75-150 sector cache; 500 sectors is sufficient
	// to defeat the cache without causing aggressive mechanical seeks
	constexpr DWORD CACHE_DEFEAT_DISTANCE = 500;

	DWORD farLBA;

	if (currentLBA > CACHE_DEFEAT_DISTANCE) {
		farLBA = currentLBA - CACHE_DEFEAT_DISTANCE;
	}
	else if (maxLBA > 0 && currentLBA + CACHE_DEFEAT_DISTANCE * 2 < maxLBA) {
		farLBA = currentLBA + CACHE_DEFEAT_DISTANCE;
	}
	else {
		farLBA = (currentLBA > CACHE_DEFEAT_DISTANCE)
			? currentLBA - CACHE_DEFEAT_DISTANCE
			: currentLBA + CACHE_DEFEAT_DISTANCE;
		if (maxLBA > 0 && farLBA >= maxLBA) {
			farLBA = maxLBA > CACHE_DEFEAT_DISTANCE ? maxLBA - CACHE_DEFEAT_DISTANCE : 0;
		}
	}

	std::vector<BYTE> buf(AUDIO_SECTOR_SIZE);
	bool ok = m_drive.ReadSectorAudioOnly(farLBA, buf.data());

	// Brief settle time so the laser assembly isn't stressed by rapid seeks
	Sleep(10);

	return ok;
}

uint32_t AudioCDCopier::HashSector(const BYTE* data, int size) {
	uint32_t hash = 2166136261u;
	for (int i = 0; i < size; i++) {
		hash ^= data[i];
		hash *= 16777619u;
	}
	return hash;
}

void AudioCDCopier::ApplyOffsetCorrection(DiscInfo& disc) {
	if (disc.driveOffset == 0 || disc.rawSectors.empty()) return;
	std::cout << "\nApplying offset correction: " << disc.driveOffset << " samples\n";

	int64_t byteOffset = static_cast<int64_t>(disc.driveOffset) * 4;

	int64_t maxOffset = static_cast<int64_t>(disc.rawSectors.size()) * AUDIO_SECTOR_SIZE;
	if (std::abs(byteOffset) >= maxOffset) {
		std::cerr << "Warning: Offset too large, skipping correction\n";
		return;
	}

	std::vector<BYTE> allAudio;
	allAudio.reserve(disc.rawSectors.size() * AUDIO_SECTOR_SIZE);

	for (auto& sec : disc.rawSectors)
		allAudio.insert(allAudio.end(), sec.begin(), sec.begin() + AUDIO_SECTOR_SIZE);

	std::vector<BYTE> corrected;
	if (byteOffset > 0) {
		corrected.assign(allAudio.begin() + static_cast<size_t>(byteOffset), allAudio.end());
		corrected.resize(allAudio.size(), 0);
	}
	else {
		corrected.resize(static_cast<size_t>(-byteOffset), 0);
		corrected.insert(corrected.end(), allAudio.begin(), allAudio.end() + byteOffset);
	}

	size_t pos = 0;
	for (auto& sec : disc.rawSectors) {
		if (pos + AUDIO_SECTOR_SIZE <= corrected.size())
			memcpy(sec.data(), &corrected[pos], AUDIO_SECTOR_SIZE);
		pos += AUDIO_SECTOR_SIZE;
	}
}