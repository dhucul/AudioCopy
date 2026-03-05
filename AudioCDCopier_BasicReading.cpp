#define NOMINMAX
#include "AudioCDCopier.h"
#include "InterruptHandler.h"
#include <iostream>
#include <conio.h>
#include <map>
#include <cstring>

// ============================================================================
// Basic Sector Reading with Retry
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
			// Restore full speed after a successful retry
			if (attempt > 0) m_drive.SetSpeed(0);
			return true;
		}

		// First failure: reduce speed for better error recovery
		if (attempt == 0) m_drive.SetSpeed(RETRY_SPEED_REDUCTION);

		// Shorter backoff for CD-R media to avoid excessive slowdown
		Sleep(5 << attempt);  // 5, 10, 20, 40, 80ms (was 10, 20, 40, 80, 160ms)
	}

	// Restore speed before returning failure — critical for CD-R to avoid
	// staying at reduced speed for subsequent good sectors
	m_drive.SetSpeed(0);
	return false;
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

		std::vector<BYTE> buf(sectorSize);
		int c2Errors = 0;
		bool ok = false;

		if (isAudio && config.useC2) {
			ScsiDrive::C2ReadOptions c2Opts;
			c2Opts.countBytes = true;

			BYTE* subchannelPtr = (sectorSize > AUDIO_SECTOR_SIZE) ? buf.data() + AUDIO_SECTOR_SIZE : nullptr;
			ok = m_drive.ReadSectorWithC2Ex(lba, buf.data(), subchannelPtr, c2Errors, nullptr, c2Opts);
			if (c2Errors > 0) result.c2ErrorPasses++;
		}
		else if (isAudio) {
			if (sectorSize > AUDIO_SECTOR_SIZE) {
				ok = m_drive.ReadSector(lba, buf.data(), buf.data() + AUDIO_SECTOR_SIZE);
			}
			else {
				ok = m_drive.ReadSectorAudioOnly(lba, buf.data());
			}
		}

		if (!ok) continue;

		// First half of passes: only accept C2-clean reads
		// Second half: accept C2-error reads for best-effort recovery
		if (c2Errors > 0 && result.c2ErrorPasses < config.maxPasses / 2) continue;

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
				result.isSecure = (result.c2ErrorPasses == 0);
				return true;
			}
		}
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
		result.isSecure = (bestCount >= config.requiredMatches && result.c2ErrorPasses == 0);
	}

	return bestCount > 0;
}