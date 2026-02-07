#define NOMINMAX
#include "AudioCDCopier.h"
#include "InterruptHandler.h"
#include "MenuHelpers.h"
#include <iostream>
#include <iomanip>
#include <vector>
#include <algorithm>
#include <chrono>
#include <cstring>

// ============================================================================
// Multi-Pass Verification & Subchannel Integrity
// ============================================================================

bool AudioCDCopier::RunMultiPassVerification(DiscInfo& disc, std::vector<MultiPassResult>& results, int passes, int scanSpeed) {
	std::cout << "\n=== Multi-Pass Verification (" << passes << " passes) ===\n";
	std::cout << "Testing read consistency with hash-based comparison...\n\n";
	results.clear();

	m_drive.SetSpeed(scanSpeed);

	DWORD totalSectors = CalculateTotalAudioSectors(disc);
	if (totalSectors == 0) {
		std::cout << "No audio tracks to verify.\n";
		return false;
	}

	int sampleInterval = std::max(1, static_cast<int>(totalSectors / 1000));
	int totalSamples = std::max(1, static_cast<int>(totalSectors / sampleInterval));
	int tested = 0, perfectMatches = 0, partialMatches = 0, failures = 0;

	std::cout << "Testing ~" << totalSamples << " sample sectors...\n";
	std::cout << "  (Press ESC or Ctrl+C to cancel)\n\n";

	ProgressIndicator progress(40);
	progress.SetLabel("  Multi-Pass");
	progress.Start();

	std::vector<std::vector<BYTE>> reads(passes, std::vector<BYTE>(AUDIO_SECTOR_SIZE));
	std::vector<uint32_t> hashes(passes);

	for (const auto& t : disc.tracks) {
		if (!t.isAudio) continue;
		DWORD start = (t.trackNumber == 1) ? 0 : t.pregapLBA;

		for (DWORD lba = start; lba <= t.endLBA; lba += sampleInterval) {
			if (g_interrupt.IsInterrupted() || g_interrupt.CheckEscapeKey()) {
				std::cout << "\n\n*** Verification cancelled by user ***\n";
				m_drive.SetSpeed(0);
				progress.Finish(false);
				return false;
			}

			bool readSuccess = true;

			for (int i = 0; i < passes; i++) {
				if (i > 0) {
					DefeatDriveCache(lba, t.endLBA);
				}

				if (!m_drive.ReadSectorAudioOnly(lba, reads[i].data())) {
					readSuccess = false;
					break;
				}

				hashes[i] = CalculateSectorHash(reads[i].data());
			}

			if (!readSuccess) {
				MultiPassResult r{};
				r.lba = lba;
				r.passesMatched = 0;
				r.totalPasses = passes;
				r.allMatch = false;
				r.majorityHash = 0;
				results.push_back(r);
				failures++;
				tested++;
				progress.Update(tested, totalSamples);
				continue;
			}

			struct HashCount { uint32_t hash; int count; };
			HashCount counts[8] = {};
			int distinctCount = 0;

			for (int i = 0; i < passes; i++) {
				bool found = false;
				for (int j = 0; j < distinctCount; j++) {
					if (counts[j].hash == hashes[i]) {
						counts[j].count++;
						found = true;
						break;
					}
				}
				if (!found && distinctCount < 8) {
					counts[distinctCount++] = { hashes[i], 1 };
				}
			}

			uint32_t majorityHash = counts[0].hash;
			int maxCount = counts[0].count;
			for (int j = 1; j < distinctCount; j++) {
				if (counts[j].count > maxCount) {
					maxCount = counts[j].count;
					majorityHash = counts[j].hash;
				}
			}

			int matchCount = 0;
			int majorityIdx = -1;
			for (int i = 0; i < passes; i++) {
				if (hashes[i] == majorityHash) {
					if (majorityIdx < 0) majorityIdx = i;
					matchCount++;
				}
			}
			if (distinctCount > 1 && majorityIdx >= 0) {
				for (int i = 0; i < passes; i++) {
					if (i == majorityIdx) continue;
					if (hashes[i] == majorityHash &&
						memcmp(reads[i].data(), reads[majorityIdx].data(), AUDIO_SECTOR_SIZE) != 0) {
						matchCount--;
					}
				}
			}

			bool allMatch = (matchCount == passes);

			MultiPassResult r{};
			r.lba = lba;
			r.passesMatched = matchCount;
			r.totalPasses = passes;
			r.allMatch = allMatch;
			r.majorityHash = majorityHash;

			if (allMatch) {
				perfectMatches++;
			}
			else if (matchCount >= (passes + 1) / 2) {
				partialMatches++;
				results.push_back(r);
			}
			else {
				failures++;
				results.push_back(r);
			}

			tested++;
			progress.Update(tested, totalSamples);
		}
	}

	progress.Finish(true);
	m_drive.SetSpeed(0);

	std::cout << "\n=== Multi-Pass Results ===\n";
	std::cout << "  Total sectors tested: " << tested << "\n";
	Console::Success("  Perfect matches: ");
	std::cout << perfectMatches << " ("
		<< (tested > 0 ? (perfectMatches * 100 / tested) : 0) << "%)\n";

	if (partialMatches > 0) {
		Console::Warning("  Partial matches: ");
		std::cout << partialMatches << "\n";
	}

	if (failures > 0) {
		Console::Error("  Failed/Inconsistent: ");
		std::cout << failures << "\n";
	}

	if (!results.empty()) {
		std::cout << "\n=== Most Inconsistent Sectors ===\n";
		std::sort(results.begin(), results.end(),
			[](const MultiPassResult& a, const MultiPassResult& b) {
				return a.passesMatched < b.passesMatched;
			});

		int shown = 0;
		for (const auto& r : results) {
			if (shown++ >= 10) break;
			if (r.passesMatched == 0 && r.majorityHash == 0)
				std::cout << "  LBA " << std::setw(6) << r.lba << ": READ FAILURE\n";
			else
				std::cout << "  LBA " << std::setw(6) << r.lba
				<< ": " << r.passesMatched << "/" << r.totalPasses
				<< " matches (hash: " << std::hex << r.majorityHash << std::dec << ")\n";
		}
	}

	return true;
}

uint32_t AudioCDCopier::CalculateSectorHash(const BYTE* data) {
	uint32_t hash = 2166136261u;
	for (int i = 0; i < AUDIO_SECTOR_SIZE; i++) {
		hash ^= data[i];
		hash *= 16777619u;
	}
	return hash;
}

bool AudioCDCopier::FlushDriveCache() {
	BYTE cdb[10] = { 0x35, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
	return m_drive.SendSCSI(cdb, 10, nullptr, 0);
}

bool AudioCDCopier::VerifySubchannelIntegrity(DiscInfo& disc, int& errorCount, int scanSpeed) {
	std::cout << "\n=== Subchannel Integrity Verification ===\n";
	errorCount = 0;

	DWORD totalSectors = 0;
	for (const auto& t : disc.tracks) {
		if (t.isAudio) totalSectors += t.endLBA - ((t.trackNumber == 1) ? 0 : t.pregapLBA) + 1;
	}

	if (totalSectors == 0) {
		std::cout << "No audio tracks to verify.\n";
		return false;
	}

	m_drive.SetSpeed(scanSpeed);
	if (scanSpeed == 0) std::cout << "Using max speed for subchannel verification...\n";
	else std::cout << "Using " << scanSpeed << "x speed for subchannel verification...\n";

	std::cout << "Verifying Q-subchannel data consistency...\n";

	ProgressIndicator progress(40);
	progress.SetLabel("  Subchannel");
	progress.Start();

	DWORD scannedSectors = 0;
	int crcErrors = 0;
	int trackMismatches = 0;
	int indexErrors = 0;

	auto startTime = std::chrono::steady_clock::now();
	auto lastSpeedUpdate = startTime;
	constexpr double CD_1X_BYTES_PER_SEC = 176400.0;

	for (const auto& t : disc.tracks) {
		if (!t.isAudio) continue;
		DWORD start = (t.trackNumber == 1) ? 0 : t.pregapLBA;

		for (DWORD lba = start; lba <= t.endLBA; lba++) {
			if (g_interrupt.IsInterrupted() || g_interrupt.CheckEscapeKey()) {
				m_drive.SetSpeed(0);
				progress.Finish(false);
				return false;
			}

			int qTrack = 0, qIndex = -1;
			if (m_drive.ReadSectorQ(lba, qTrack, qIndex)) {
				bool trackOk = (qTrack == t.trackNumber) ||
					(qTrack == 0) ||
					(lba <= t.startLBA && qTrack == t.trackNumber - 1);
				if (!trackOk) {
					trackMismatches++;
					errorCount++;
				}

				if (qIndex < 0 || qIndex > 99) {
					indexErrors++;
					errorCount++;
				}
			}
			else {
				crcErrors++;
				errorCount++;
			}

			scannedSectors++;

			auto now = std::chrono::steady_clock::now();
			auto timeSinceUpdate = std::chrono::duration<double>(now - lastSpeedUpdate).count();

			if (timeSinceUpdate >= 0.5) {
				double totalElapsed = std::chrono::duration<double>(now - startTime).count();
				if (totalElapsed > 0) {
					double bytesRead = scannedSectors * 2352.0;
					double actualSpeedX = bytesRead / (totalElapsed * CD_1X_BYTES_PER_SEC);

					char speedLabel[64];
					snprintf(speedLabel, sizeof(speedLabel), "  Subchannel [%.1fx]", actualSpeedX);
					progress.SetLabel(speedLabel);
				}
				lastSpeedUpdate = now;
			}

			progress.Update(static_cast<int>(scannedSectors), static_cast<int>(totalSectors));
		}
	}

	progress.Finish(true);
	m_drive.SetSpeed(0);

	auto endTime = std::chrono::steady_clock::now();
	double totalSeconds = std::chrono::duration<double>(endTime - startTime).count();
	double avgSpeedX = 0.0;
	if (totalSeconds > 0) {
		double totalBytes = scannedSectors * 2352.0;
		avgSpeedX = totalBytes / (totalSeconds * CD_1X_BYTES_PER_SEC);
	}

	std::cout << "\n=== Speed Verification ===\n";
	std::cout << "Requested speed: " << (scanSpeed == 0 ? "Max" : std::to_string(scanSpeed) + "x") << "\n";
	std::cout << "Measured throughput: " << std::fixed << std::setprecision(1) << avgSpeedX << "x\n";

	WORD actualRead = 0, actualWrite = 0;
	if (m_drive.GetActualSpeed(actualRead, actualWrite)) {
		double reportedSpeedX = actualRead / 176.4;
		std::cout << "Drive-reported speed: " << std::fixed << std::setprecision(1)
			<< reportedSpeedX << "x (" << actualRead << " KB/s)\n";

		if (scanSpeed > 0 && avgSpeedX > scanSpeed * 1.5) {
			std::cout << "** Warning: Drive may be ignoring speed limit **\n";
		}
	}

	std::cout << "\n=== Subchannel Verification Results ===\n";
	std::cout << "Sectors verified: " << scannedSectors << "\n";
	std::cout << "CRC/Read errors: " << crcErrors << "\n";
	std::cout << "Track mismatches: " << trackMismatches << "\n";
	std::cout << "Index errors: " << indexErrors << "\n";
	std::cout << "Total errors: " << errorCount << "\n";
	std::cout << "Scan time: " << std::fixed << std::setprecision(1) << totalSeconds << " seconds\n";

	if (errorCount == 0) {
		std::cout << "*** Subchannel data is CLEAN ***\n";
	}
	else {
		std::cout << "*** Subchannel errors detected - may affect accurate ripping ***\n";
	}

	return true;
}