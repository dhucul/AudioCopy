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

	EnsureCapabilitiesDetected();

	m_drive.SetSpeed(scanSpeed);

	DWORD totalSectors = CalculateTotalAudioSectors(disc);
	if (totalSectors == 0) {
		std::cout << "No audio tracks to verify.\n";
		return false;
	}

	int sampleInterval = std::max(1, static_cast<int>(totalSectors / 1000));

	// Count expected samples accurately (sampling restarts per track)
	int totalSamples = 0;
	for (const auto& t : disc.tracks) {
		if (!t.isAudio) continue;
		DWORD start = (t.trackNumber == 1) ? 0 : t.pregapLBA;
		DWORD trackSectors = t.endLBA - start + 1;
		totalSamples += static_cast<int>((trackSectors + sampleInterval - 1) / sampleInterval);
	}
	totalSamples = std::max(1, totalSamples);

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
				if (i > 0 && !m_hasAccurateStream) {
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
			HashCount counts[16] = {};
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
				if (!found && distinctCount < 16) {
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
		<< std::fixed << std::setprecision(1)
		<< (tested > 0 ? (perfectMatches * 100.0 / tested) : 0.0) << "%)\n";

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

	DWORD totalSectors = CalculateTotalAudioSectors(disc);

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
	int indexWarnings = 0;
	int consecutiveErrors = 0;
	int peakBurst = 0;
	DWORD peakBurstStartLBA = 0;
	DWORD currentBurstStartLBA = 0;

	// Early-abort: stop after this many consecutive errors to avoid grinding
	// through a severely damaged or unreadable region indefinitely.
	constexpr int CONSECUTIVE_ERROR_ABORT = 200;
	bool abortedEarly = false;

	auto startTime = std::chrono::steady_clock::now();
	auto lastSpeedUpdate = startTime;
	constexpr double CD_1X_BYTES_PER_SEC = 176400.0;

	for (const auto& t : disc.tracks) {
		if (!t.isAudio) continue;
		if (abortedEarly) break;
		DWORD start = (t.trackNumber == 1) ? 0 : t.pregapLBA;

		for (DWORD lba = start; lba <= t.endLBA; lba++) {
			if (g_interrupt.IsInterrupted() || g_interrupt.CheckEscapeKey()) {
				m_drive.SetSpeed(0);
				progress.Finish(false);
				return false;
			}

			// Early-abort on sustained unreadable regions
			if (consecutiveErrors >= CONSECUTIVE_ERROR_ABORT) {
				abortedEarly = true;
				Console::Warning("\nAborting: ");
				std::cout << CONSECUTIVE_ERROR_ABORT
					<< " consecutive errors starting at LBA " << currentBurstStartLBA
					<< " - disc may be severely damaged.\n";
				break;
			}

			bool sectorError = false;
			int qTrack = 0, qIndex = -1;

			// Adaptive read: single read mid-track, majority voting near transitions
			if (m_drive.ReadSectorQAdaptive(lba, qTrack, qIndex, t.pregapLBA, t.startLBA)) {
				// Validate track number:
				// - Within track: must match current track
				// - At pregap: allow current track (index 00) or previous track
				//   (pregap may read old track due to drive latency)
				bool isInPregap = (lba >= t.pregapLBA && lba < t.startLBA);
				bool trackOk = (qTrack == t.trackNumber) ||
					(isInPregap && qTrack == t.trackNumber - 1);

				if (!trackOk) {
					trackMismatches++;
					errorCount++;
					sectorError = true;
				}

				// Index range: 0-99 (0 = pregap, 1-99 = track data)
				if (qIndex < 0 || qIndex > 99) {
					indexErrors++;
					errorCount++;
					sectorError = true;
				}
				// Track index vs region consistency (informational, not an error)
				else {
					bool shouldBeIndex0 = isInPregap;
					bool isIndex0 = (qIndex == 0);
					if (shouldBeIndex0 != isIndex0) {
						indexWarnings++;
					}
				}
			}
			else {
				crcErrors++;
				errorCount++;
				sectorError = true;
			}

			if (sectorError) {
				if (consecutiveErrors == 0) currentBurstStartLBA = lba;
				consecutiveErrors++;
				if (consecutiveErrors > peakBurst) {
					peakBurst = consecutiveErrors;
					peakBurstStartLBA = currentBurstStartLBA;
				}
			}
			else {
				consecutiveErrors = 0;
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

	progress.Finish(!abortedEarly);
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
	std::cout << "Sectors verified: " << scannedSectors;
	if (abortedEarly) std::cout << " (aborted early)";
	std::cout << "\n";
	std::cout << "CRC/Read errors: " << crcErrors << "\n";
	std::cout << "Track mismatches: " << trackMismatches << "\n";
	std::cout << "Index errors: " << indexErrors << "\n";
	if (indexWarnings > 0) {
		std::cout << "Index warnings: " << indexWarnings << " (near transition boundaries, informational)\n";
	}
	std::cout << "Total errors: " << errorCount << "\n";
	std::cout << "Scan time: " << std::fixed << std::setprecision(1) << totalSeconds << " seconds\n";

	if (errorCount == 0) {
		std::cout << "*** Subchannel data is CLEAN ***\n";
	}
	else {
		std::cout << "*** Subchannel errors detected - may affect accurate ripping ***\n";
	}

	if (peakBurst > 1) {
		std::cout << "Peak error burst: " << peakBurst
			<< " sectors starting at LBA " << peakBurstStartLBA << "\n";
	}

	// ── Error rate interpretation ──────────────────────────────────────
	double errorRate = (scannedSectors > 0)
		? (static_cast<double>(errorCount) / scannedSectors) * 100.0
		: 0.0;

	std::cout << "\n=== Interpreting Results ===\n";
	std::cout << "Error rate: " << std::fixed << std::setprecision(2) << errorRate << "%\n";

	std::cout << std::left << std::setw(14) << "Error rate"
		<< std::setw(50) << "Assessment" << "\n";
	std::cout << std::string(64, '-') << "\n";

	if (errorRate == 0.0) {
		Console::Success("0%            ");
		std::cout << "Exceptionally clean - uncommon even on new discs\n";
	}
	else if (errorRate < 1.0) {
		Console::Success("< 1%          ");
		std::cout << "Excellent - no impact on ripping\n";
	}
	else if (errorRate <= 3.0) {
		Console::Info("1-3%          ");
		std::cout << "Normal - typical baseline for most CDs and drives\n";
	}
	else if (errorRate <= 5.0) {
		Console::Warning("3-5%          ");
		std::cout << "Elevated - may indicate disc wear, but audio extraction is unaffected\n";
	}
	else if (errorRate <= 10.0) {
		Console::Warning("5-10%         ");
		std::cout << "High - disc surface may be degraded; cross-reference with C2 scan\n";
	}
	else {
		Console::Error("> 10%         ");
		std::cout << "Severe - likely physical damage; prioritize backup with secure rip mode\n";
	}

	std::cout << "\nYour disc: " << std::fixed << std::setprecision(2) << errorRate << "% -> ";
	if (errorRate == 0.0)       Console::Success("EXCEPTIONAL\n");
	else if (errorRate < 1.0)   Console::Success("EXCELLENT\n");
	else if (errorRate <= 3.0)  Console::Info("NORMAL\n");
	else if (errorRate <= 5.0)  Console::Warning("ELEVATED\n");
	else if (errorRate <= 10.0) Console::Warning("HIGH\n");
	else                        Console::Error("SEVERE\n");

	return true;
}

// ============================================================================
// Subchannel Burn Status Verification
//
// Samples sectors across the disc and inspects the raw 96-byte subchannel
// block to determine whether subchannel data was genuinely mastered/burned.
// Checks Q-channel CRC, P-channel state, R-W non-zero data, and MSF timing.
// ============================================================================

// Helper: Red Book stores Q-channel CRC as ones-complement.  Accept both
// the inverted form (per spec) and the non-inverted form (some drives
// return the CRC already corrected).
static bool IsQCrcValid(uint16_t calcCrc, uint16_t storedCrc) {
	return (calcCrc == static_cast<uint16_t>(~storedCrc))  // Red Book standard (inverted)
		|| (calcCrc == storedCrc);                          // Some drives return non-inverted
}

bool AudioCDCopier::VerifySubchannelBurnStatus(DiscInfo& disc, SubchannelBurnResult& result, int scanSpeed) {
	std::cout << "\n=== Subchannel Burn Status Verification ===\n";
	result = {};

	// ── Detect media type ───────────────────────────────────────────────
	// Identifies pressed (CD-ROM) vs burned (CD-R/CD-RW) media so the
	// verdict can provide context-specific guidance.
	m_drive.GetMediaProfile(result.mediaProfile, result.mediaTypeName);
	if (!result.mediaTypeName.empty()) {
		std::cout << "Media type detected: " << result.mediaTypeName << "\n";
	}

	DWORD totalSectors = CalculateTotalAudioSectors(disc);
	if (totalSectors == 0) {
		std::cout << "No audio tracks to verify.\n";
		return false;
	}

	// Sample ~500 sectors spread evenly across the disc for a fast but
	// representative check.  Minimum interval of 1 to avoid division by zero.
	constexpr int TARGET_SAMPLES = 500;
	int sampleInterval = std::max(1, static_cast<int>(totalSectors / TARGET_SAMPLES));
	int expectedSamples = std::max(1, static_cast<int>(totalSectors / sampleInterval));

	m_drive.SetSpeed(scanSpeed);
	if (scanSpeed == 0) std::cout << "Using max speed...\n";
	else std::cout << "Using " << scanSpeed << "x speed...\n";

	std::cout << "Sampling ~" << expectedSamples << " sectors across the disc...\n";
	std::cout << "  (Press ESC or Ctrl+C to cancel)\n\n";

	ProgressIndicator progress(40);
	progress.SetLabel("  Burn Check");
	progress.Start();

	std::vector<BYTE> audioBuf(AUDIO_SECTOR_SIZE);
	std::vector<BYTE> subBuf(SUBCHANNEL_SIZE);

	int prevAbsMin = -1, prevAbsSec = -1, prevAbsFrame = -1;

	for (const auto& t : disc.tracks) {
		if (!t.isAudio) continue;
		DWORD start = (t.trackNumber == 1) ? 0 : t.pregapLBA;

		// Reset MSF tracking at each track boundary so non-contiguous
		// audio regions (e.g. data tracks interspersed) don't produce
		// a spurious MSF-timing failure on the first sample.
		prevAbsMin = -1;
		prevAbsSec = -1;
		prevAbsFrame = -1;

		for (DWORD lba = start; lba <= t.endLBA; lba += sampleInterval) {
			if (g_interrupt.IsInterrupted() || g_interrupt.CheckEscapeKey()) {
				m_drive.SetSpeed(0);
				progress.Finish(false);
				return false;
			}

			result.totalSampled++;

			if (!m_drive.ReadSector(lba, audioBuf.data(), subBuf.data())) {
				result.readFailures++;
				progress.Update(result.totalSampled, expectedSamples);
				continue;
			}

			// ── Check if entire subchannel block is zero ────────────────
			bool allZero = true;
			for (int i = 0; i < SUBCHANNEL_SIZE; i++) {
				if (subBuf[i] != 0) { allZero = false; break; }
			}
			if (allZero) {
				result.emptySubchannel++;
				progress.Update(result.totalSampled, expectedSamples);
				continue;
			}

			// ── De-interleave channels from raw 96 bytes ────────────────
			// Each of the 96 bytes carries one bit per channel:
			//   bit 7 = P, bit 6 = Q, bits 5-0 = R through W
			BYTE pChannel[12] = {};
			BYTE qChannel[12] = {};
			int rwNonZeroBits = 0;

			for (int i = 0; i < 96; i++) {
				int byteIdx = i / 8;
				int bitIdx = 7 - (i % 8);
				if (subBuf[i] & 0x80) pChannel[byteIdx] |= (1 << bitIdx);
				if (subBuf[i] & 0x40) qChannel[byteIdx] |= (1 << bitIdx);

				// Count non-zero R-W bits for density check
				BYTE rw = subBuf[i] & 0x3F;
				if (rw) {
					// Popcount of the 6 R-W bits
					for (BYTE b = rw; b; b &= b - 1) rwNonZeroBits++;
				}
			}

			// Require a minimum bit density to filter single-bit noise.
			// 576 total R-W bits per sector; real data sets dozens of bits,
			// noise typically flips 1-5.  Threshold of 12 bits (~2%) filters
			// stray bit errors while allowing even sparse CD-G frames.
			constexpr int RW_BIT_DENSITY_THRESHOLD = 12;
			if (rwNonZeroBits >= RW_BIT_DENSITY_THRESHOLD) {
				result.rwDataPresent++;

				// ── CD-G pack structure validation ───────────────────────
				// R-W data forms 96 six-bit symbols per sector, grouped into
				// 4 packs of 24 symbols.  Symbol 0 of each pack is the
				// command byte; CD-G uses command 0x09.
				int cdgPacks = 0;
				for (int pack = 0; pack < 4; pack++) {
					BYTE cmd = subBuf[pack * 24] & 0x3F;
					if (cmd == 0x09) cdgPacks++;
				}
				if (cdgPacks > 0) result.cdgPacketsFound++;
			}

			// ── Validate Q-channel CRC-16 ───────────────────────────────
			// Red Book stores the CRC as its ones-complement; IsQCrcValid()
			// accepts both inverted and non-inverted forms for compatibility.
			uint16_t calcCrc = SubchannelCRC16(qChannel, 10);
			uint16_t storedCrc = (static_cast<uint16_t>(qChannel[10]) << 8) | qChannel[11];
			if (IsQCrcValid(calcCrc, storedCrc)) {
				result.validQCrc++;

				// Only ADR=1 frames carry position data with MSF timing
				BYTE adr = qChannel[0] & 0x0F;
				if (adr == 1) {
					// Absolute MSF is stored in Q bytes 7-9 (BCD)
					int absMin = BcdToBin(qChannel[7]);
					int absSec = BcdToBin(qChannel[8]);
					int absFrame = BcdToBin(qChannel[9]);

					// Verify MSF increments since the last sampled sector
					if (prevAbsMin >= 0) {
						int prevTotal = prevAbsMin * 4500 + prevAbsSec * 75 + prevAbsFrame;
						int curTotal = absMin * 4500 + absSec * 75 + absFrame;
						if (curTotal > prevTotal) {
							result.validMsfTiming++;
						}
					}
					prevAbsMin = absMin;
					prevAbsSec = absSec;
					prevAbsFrame = absFrame;
				}
			}
			else {
				result.invalidQCrc++;
			}

			// ── P-channel check ─────────────────────────────────────────
			// In the audio region (INDEX 01+) P should be all-zero (play).
			// In the pre-gap (INDEX 00) P should be all-one (pause).
			bool isInPregap = (lba >= t.pregapLBA && lba < t.startLBA);
			BYTE expectedP = isInPregap ? 0xFF : 0x00;
			bool pOk = true;
			for (int i = 0; i < 12; i++) {
				if (pChannel[i] != expectedP) { pOk = false; break; }
			}
			if (pOk) result.pChannelCorrect++;

			progress.Update(result.totalSampled, expectedSamples);
		}
	}

	progress.Finish(true);
	m_drive.SetSpeed(0);

	// ── Compute verdict ─────────────────────────────────────────────────
	// Denominators must exclude the right categories to avoid dilution:
	//   - CRC %  → only sectors where CRC was actually tested (non-empty)
	//   - Empty% → only sectors where the read succeeded
	int successfulReads = result.totalSampled - result.readFailures;
	int crcTestedSectors = result.validQCrc + result.invalidQCrc;

	if (crcTestedSectors > 0) {
		result.qCrcValidPercent =
			(static_cast<double>(result.validQCrc) / crcTestedSectors) * 100.0;
	}

	double emptyPercent = (successfulReads > 0)
		? (static_cast<double>(result.emptySubchannel) / successfulReads) * 100.0
		: 100.0;

	// ── Formatted-Q probe ───────────────────────────────────────────────
	// If most raw subchannel reads came back empty (or all reads failed),
	// the drive may not support raw P-W subchannel but CAN read formatted
	// Q via a different SCSI sub-channel selector.  Probe a few sectors
	// with ReadSectorQSingle (raw → formatted fallback) to distinguish
	// "subchannel not burned" from "drive can't return raw subchannel".
	if (emptyPercent > 50.0 || successfulReads == 0) {
		int probeOk = 0, probeAttempts = 0;
		for (const auto& t : disc.tracks) {
			if (!t.isAudio || probeAttempts >= 5) break;
			DWORD mid = t.startLBA + (t.endLBA - t.startLBA) / 2;
			int qTrack = 0, qIndex = 0;
			probeAttempts++;
			if (m_drive.ReadSectorQSingle(mid, qTrack, qIndex))
				probeOk++;
		}

		if (probeOk > 0) {
			// Formatted Q works — Q-channel data is on the disc but the
			// drive cannot return raw P-W subchannel.  Since we can't read
			// raw R-W data, we cannot determine if CD-G/CD-TEXT is present.
			bool isBurnedMedia = (result.mediaProfile == 0x0009 || result.mediaProfile == 0x000A);
			result.subchannelBurned = false;

			if (isBurnedMedia) {
				result.verdict =
					"STANDARD BURNED CD - Q-channel timing data verified via formatted mode.\n"
					"           This drive does not support raw subchannel reading, so R-W content\n"
					"           (CD-G, CD-TEXT) cannot be verified.\n"
					"           P+Q timing data is always written by the drive automatically.";
			}
			else {
				result.verdict =
					"STANDARD PRESSED CD - Q-channel timing data verified via formatted mode.\n"
					"           This drive does not support raw subchannel reading, so R-W content\n"
					"           (CD-G, CD-TEXT) cannot be verified.";
			}

			PrintSubchannelBurnReport(result);
			return true;
		}
	}

	// ── Standard verdict from raw subchannel analysis ───────────────────
	int nonEmpty = successfulReads - result.emptySubchannel;
	double rwPercent = (nonEmpty > 0)
		? (static_cast<double>(result.rwDataPresent) / nonEmpty) * 100.0
		: 0.0;
	bool hasRWContent = (rwPercent >= 25.0);
	bool isBurnedMedia = (result.mediaProfile == 0x0009 || result.mediaProfile == 0x000A);

	if (result.qCrcValidPercent >= 90.0 && emptyPercent < 5.0) {
		if (hasRWContent) {
			result.subchannelBurned = true;
			result.verdict =
				"CONFIRMED - Full subchannel data was burned to disc.\n"
				"           Includes R-W content (CD-G graphics, CD-TEXT, or similar).";
		}
		else if (isBurnedMedia) {
			result.subchannelBurned = false;
			result.verdict =
				"STANDARD BURNED CD - Basic timing data (P+Q) is healthy.\n"
				"           No CD-G or CD-TEXT content was found in the R-W channels.\n"
				"           The \"burn with subchannel\" option only copies what exists on the source.";
		}
		else {
			result.subchannelBurned = false;
			result.verdict =
				"STANDARD PRESSED CD - Basic timing data (P+Q) is healthy.\n"
				"           No CD-G or CD-TEXT content was found in the R-W channels.\n"
				"           This is normal for most factory-pressed audio CDs.";
		}
	}
	else if (result.qCrcValidPercent >= 50.0 && emptyPercent < 30.0) {
		if (hasRWContent) {
			result.subchannelBurned = true;
			result.verdict =
				"CONFIRMED - Full subchannel data was burned to disc.\n"
				"           Includes R-W content (CD-G graphics, CD-TEXT, or similar).\n"
				"           Some timing errors were detected, which is normal for burned (CD-R) media.";
		}
		else if (isBurnedMedia) {
			result.subchannelBurned = false;
			result.verdict =
				"STANDARD BURNED CD - Basic timing data (P+Q) is present with some CRC errors.\n"
				"           This is normal for CD-R media and does not affect audio quality.\n"
				"           No CD-G or CD-TEXT content was found in the R-W channels.\n"
				"           The \"burn with subchannel\" option only copies what exists on the source.";
		}
		else {
			result.subchannelBurned = false;
			result.verdict =
				"STANDARD PRESSED CD - Basic timing data (P+Q) is present with some CRC errors.\n"
				"           This may indicate minor disc wear but does not affect audio quality.\n"
				"           No CD-G or CD-TEXT content was found in the R-W channels.";
		}
	}
	else if (emptyPercent >= 80.0) {
		result.subchannelBurned = false;
		result.verdict =
			"EMPTY - No subchannel data found on this disc.\n"
			"           The disc was burned without any subchannel writing.";
	}
	else {
		result.subchannelBurned = false;
		result.verdict =
			"INCONCLUSIVE - Subchannel data could not be reliably read.\n"
			"           This may indicate a low-quality burn, disc damage, or a drive\n"
			"           that does not support raw subchannel reading.";
	}

	PrintSubchannelBurnReport(result);
	return true;
}

void AudioCDCopier::PrintSubchannelBurnReport(const SubchannelBurnResult& result) {

	// ── Plain-language explanation ───────────────────────────────────────
	std::cout << "\n=== What Is Subchannel Data? ===\n";
	std::cout << "Every audio CD contains hidden data alongside the music:\n";
	std::cout << "  P+Q channels  - Track numbers, timing, and pause markers.\n";
	std::cout << "                  These are ALWAYS written by the drive automatically.\n";
	std::cout << "  R-W channels  - Optional extra data such as CD-G karaoke graphics\n";
	std::cout << "                  or CD-TEXT (artist/title info). Only present if the\n";
	std::cout << "                  source disc contained them AND they were burned.\n";

	std::cout << "\n=== Subchannel Burn Status Report ===\n";
	if (!result.mediaTypeName.empty()) {
		std::cout << "Media type:            " << result.mediaTypeName;
		if (result.mediaProfile == 0x0008)      std::cout << " (factory-pressed)";
		else if (result.mediaProfile == 0x0009) std::cout << " (recordable)";
		else if (result.mediaProfile == 0x000A) std::cout << " (rewritable)";
		std::cout << "\n";
	}
	std::cout << "Sectors sampled:       " << result.totalSampled << "\n";
	std::cout << "Read failures:         " << result.readFailures << "\n";
	std::cout << "Empty subchannel:      " << result.emptySubchannel << "\n";
	std::cout << "Valid Q-channel CRC:   " << result.validQCrc
		<< " (" << std::fixed << std::setprecision(1) << result.qCrcValidPercent << "%)\n";
	std::cout << "Invalid Q-channel CRC: " << result.invalidQCrc << "\n";
	std::cout << "P-channel correct:     " << result.pChannelCorrect << "\n";
	std::cout << "Valid MSF timing:      " << result.validMsfTiming << "\n";

	// ── R-W / CD-G classification ───────────────────────────────────────
	int nonEmpty = result.totalSampled - result.readFailures - result.emptySubchannel;
	double rwPercent = (nonEmpty > 0)
		? (static_cast<double>(result.rwDataPresent) / nonEmpty) * 100.0
		: 0.0;
	double cdgPercent = (nonEmpty > 0)
		? (static_cast<double>(result.cdgPacketsFound) / nonEmpty) * 100.0
		: 0.0;

	std::cout << "R-W data present:      " << result.rwDataPresent;
	if (result.rwDataPresent == 0) {
		std::cout << " (none - standard audio CD)";
	}
	else if (cdgPercent >= 50.0) {
		std::cout << " (" << std::fixed << std::setprecision(1) << cdgPercent
			<< "% verified CD-G - disc contains karaoke/graphics data)";
	}
	else if (cdgPercent >= 10.0) {
		std::cout << " (" << std::fixed << std::setprecision(1) << cdgPercent
			<< "% verified CD-G - partial graphics data detected)";
	}
	else if (rwPercent < 10.0) {
		std::cout << " (negligible - likely electrical noise from the drive, not real data)";
	}
	else {
		std::cout << " (non-graphics R-W data detected - possibly CD-TEXT metadata)";
	}
	std::cout << "\n";

	std::cout << "\n";
	if (result.subchannelBurned) {
		Console::Success("Verdict: ");
	}
	else {
		Console::Info("Verdict: ");
	}
	std::cout << result.verdict << "\n";

	// ── Recommendations ─────────────────────────────────────────────────
	bool isBurnedMedia = (result.mediaProfile == 0x0009 || result.mediaProfile == 0x000A);

	std::cout << "\n=== Recommendations ===\n";
	if (result.subchannelBurned && result.qCrcValidPercent >= 90.0) {
		Console::Success("This disc has subchannel data worth preserving.\n");
		Console::Info("  - Enable subchannel extraction when ripping this disc.\n");
		Console::Info("  - Q-channel timing provides accurate track index positions.\n");
		if (result.cdgPacketsFound > 0) {
			Console::Info("  - CD-G graphics detected: extract R-W channels to preserve them.\n");
		}
	}
	else if (result.subchannelBurned) {
		Console::Success("This disc has subchannel data, but quality is reduced (normal for CD-R).\n");
		Console::Info("  - Subchannel extraction is still recommended to preserve R-W content.\n");
		Console::Info("  - For track boundaries, TOC-based indexing may be more reliable than\n");
		Console::Info("    raw Q-channel timing on this disc.\n");
	}
	else {
		Console::Info("This disc has no extra subchannel content to extract.\n");
		Console::Info("  - Subchannel extraction can be skipped when ripping.\n");
		Console::Info("  - Track boundaries will be read from the disc's table of contents (TOC).\n");
		if (isBurnedMedia) {
			Console::Info("\n");
			Console::Info("  Note: If you burned this disc with subchannel writing enabled but see\n");
			Console::Info("  no R-W content, the source disc did not have CD-G or CD-TEXT data.\n");
			Console::Info("  For standard audio CDs, \"burn with subchannel\" and \"burn without\n");
			Console::Info("  subchannel\" produce the same result because the R-W channels are empty\n");
			Console::Info("  on the source. The P+Q timing data is always written by the drive.\n");
		}
	}
}