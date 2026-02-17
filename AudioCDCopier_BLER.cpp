#define NOMINMAX
#include "AudioCDCopier.h"
#include "InterruptHandler.h"
#include <iostream>
#include <iomanip>
#include <vector>
#include <algorithm>
#include <cstring>

// ============================================================================
// BLER Quality Scanning
// ============================================================================

bool AudioCDCopier::RunBlerScan(const DiscInfo& disc, BlerResult& result, int scanSpeed) {
	std::cout << "\n=== CD BLER Quality Scan ===\n";

	if (!m_drive.CheckC2Support()) {
		std::cout << "ERROR: C2 not supported.\n";
		return false;
	}

	// Check if C1 block error data is available (ErrorPointers or PlextorD8 mode)
	bool hasC1Support = m_drive.SupportsC1BlockErrors();

	if (hasC1Support) {
		std::cout << "C1 block error reporting available — C1 and C2 errors will be reported.\n\n";
	}
	else {
		std::cout << "Note: C1 errors are not available in this drive's C2 mode.\n";
		std::cout << "      This scan reports C2 (uncorrectable) errors only.\n\n";
	}

	DWORD totalSectors = 0, firstLBA = 0, lastLBA = 0;
	bool first = true;
	for (const auto& t : disc.tracks) {
		if (t.isAudio) {
			DWORD start = (t.trackNumber == 1) ? 0 : t.pregapLBA;
			if (first) { firstLBA = start; first = false; }
			lastLBA = t.endLBA;
			totalSectors += t.endLBA - start + 1;
		}
	}

	if (totalSectors == 0) {
		std::cout << "No audio tracks.\n";
		return false;
	}

	result = BlerResult{};
	result.totalSectors = totalSectors;
	result.totalSeconds = (totalSectors + 74) / 75;
	result.perSecondC2.resize(result.totalSeconds + 1, { 0, 0 });
	result.hasC1Data = hasC1Support;
	if (hasC1Support) {
		result.perSecondC1.resize(result.totalSeconds + 1, { 0, 0 });
	}

	std::cout << "Scanning " << totalSectors << " sectors...\n";
	std::cout << "  (Press ESC or Ctrl+C to cancel)\n\n";
	m_drive.SetSpeed(scanSpeed);

	DWORD scannedSectors = 0;
	int currentErrorRun = 0;

	// Track error LBAs for cluster analysis
	std::vector<DWORD> errorLBAs;

	ProgressIndicator progress(40);
	progress.SetLabel("  BLER Scan");
	progress.Start();

	ScsiDrive::C2ReadOptions c2Opts;
	c2Opts.multiPass = false;
	c2Opts.countBytes = true;
	c2Opts.defeatCache = true;

	std::vector<BYTE> c2Buffer(C2_ERROR_SIZE, 0);

	for (const auto& t : disc.tracks) {
		if (!t.isAudio) continue;
		DWORD start = (t.trackNumber == 1) ? 0 : t.pregapLBA;

		for (DWORD lba = start; lba <= t.endLBA; lba++) {
			if (g_interrupt.IsInterrupted() || g_interrupt.CheckEscapeKey()) {
				m_drive.SetSpeed(0);
				std::cout << "\n\n*** Scan cancelled by user ***\n";
				return false;
			}

			std::vector<BYTE> buf(AUDIO_SECTOR_SIZE);
			int c2Errors = 0;
			int c1BlockErrors = 0, c2BlockErrors = 0;
			BYTE senseKey = 0, asc = 0, ascq = 0;
			std::memset(c2Buffer.data(), 0, C2_ERROR_SIZE);

			// Use audio-relative sector count for indexing to handle
			// mixed-mode discs with non-audio gaps between tracks
			size_t secIdx = static_cast<size_t>(scannedSectors / 75);
			if (secIdx >= result.perSecondC2.size())
				secIdx = result.perSecondC2.size() - 1;

			// Record the starting LBA for each time bucket
			if (scannedSectors % 75 == 0) {
				result.perSecondC2[secIdx].first = static_cast<int>(lba);
				if (hasC1Support)
					result.perSecondC1[secIdx].first = static_cast<int>(lba);
			}

			// Always use ReadSectorWithC2Ex — it respects m_c2Mode and
			// routes to PlextorReadC2 internally when D8 is the active mode.
			bool readSuccess = m_drive.ReadSectorWithC2Ex(
				lba, buf.data(), nullptr, c2Errors, c2Buffer.data(), c2Opts,
				&senseKey, &asc, &ascq,
				hasC1Support ? &c1BlockErrors : nullptr,
				hasC1Support ? &c2BlockErrors : nullptr);

			// FIX 1: Track per-sector error flag for zone classification
			// (0 = no error, 1 = error) — avoids inflating zone rates with
			// raw C2 counts and ensures every sector is classified into a zone
			int zoneError = 0;

			if (readSuccess) {
				bool recovered = (senseKey == 0x01);

				// ── Collect C1 block errors (when available) ──
				if (hasC1Support && c1BlockErrors > 0) {
					result.totalC1Errors += c1BlockErrors;
					result.totalC1Sectors++;
					result.perSecondC1[secIdx].second += c1BlockErrors;

					if (c1BlockErrors > result.maxC1InSingleSector) {
						result.maxC1InSingleSector = c1BlockErrors;
						result.worstC1SectorLBA = lba;
					}
				}

				if (c2Errors > 0) {
					if (!recovered) {
						result.totalC2Errors += c2Errors;
						result.totalC2Sectors++;
						result.perSecondC2[secIdx].second += c2Errors;

						if (c2Errors > result.maxC2InSingleSector) {
							result.maxC2InSingleSector = c2Errors;
							result.worstSectorLBA = lba;
						}

						zoneError = 1;
						errorLBAs.push_back(lba);

						currentErrorRun++;
						if (currentErrorRun > result.consecutiveErrorSectors) {
							result.consecutiveErrorSectors = currentErrorRun;
						}
					}
					else {
						// Recovered errors — drive corrected internally.
						// Not counted as C2 failures but the run is broken.
						currentErrorRun = 0;
					}
				}
				else {
					// No C2 errors — reset the consecutive error run
					currentErrorRun = 0;
				}
			}
			else {
				result.totalReadFailures++;

				zoneError = 1;
				errorLBAs.push_back(lba);

				currentErrorRun++;
				if (currentErrorRun > result.consecutiveErrorSectors) {
					result.consecutiveErrorSectors = currentErrorRun;
				}
			}

			// FIX 2: Use LBA-based zone classification via the shared helper
			// so zones map to physical disc position (consistent with DiscRot scan)
			ClassifyZone(lba, firstLBA, lastLBA, zoneError, result.zoneStats);

			scannedSectors++;
			progress.Update(static_cast<int>(scannedSectors), static_cast<int>(totalSectors));
		}
	}

	progress.Finish(true);
	m_drive.SetSpeed(0);

	result.avgC2PerSecond = result.totalSeconds > 0
		? static_cast<double>(result.totalC2Errors) / result.totalSeconds : 0;

	if (hasC1Support) {
		result.avgC1PerSecond = result.totalSeconds > 0
			? static_cast<double>(result.totalC1Errors) / result.totalSeconds : 0;
	}

	for (size_t i = 0; i < result.perSecondC2.size(); i++) {
		if (result.perSecondC2[i].second > result.maxC2PerSecond) {
			result.maxC2PerSecond = result.perSecondC2[i].second;
			result.worstSecondLBA = result.perSecondC2[i].first;
		}
	}

	if (hasC1Support) {
		for (size_t i = 0; i < result.perSecondC1.size(); i++) {
			if (result.perSecondC1[i].second > result.maxC1PerSecond) {
				result.maxC1PerSecond = result.perSecondC1[i].second;
			}
		}
	}

	// --- Build error clusters using adaptive tolerance ---
	if (!errorLBAs.empty()) {
		std::sort(errorLBAs.begin(), errorLBAs.end());

		// FIX 3: Use the shared DetectErrorClusters helper with speed-adaptive
		// tolerance instead of strict adjacency (gap of 1).  At higher read
		// speeds error scatter is wider, so strict adjacency fragments single
		// damage regions into many tiny clusters.
		DetectErrorClusters(errorLBAs, result.errorClusters, scanSpeed);

		// Largest cluster size
		for (const auto& c : result.errorClusters) {
			if (c.size() > result.largestClusterSize)
				result.largestClusterSize = c.size();
		}

		// FIX 4: Edge concentration — use rate-based comparison with an
		// absolute threshold (consistent with DiscRot AnalyzeErrorPatterns).
		double innerRate = result.zoneStats.InnerErrorRate();
		double middleRate = result.zoneStats.MiddleErrorRate();
		double outerRate = result.zoneStats.OuterErrorRate();
		result.hasEdgeConcentration =
			(outerRate > innerRate * 2.0 && outerRate > 1.0) ||
			(innerRate > outerRate * 2.0 && innerRate > 1.0);

		// FIX 5: Progressive pattern — require monotonic increase across all
		// three zones (inner < middle < outer).
		result.hasProgressivePattern =
			innerRate < middleRate &&
			middleRate < outerRate &&
			outerRate > 0.5;
	}

	if (result.totalReadFailures > 0) result.qualityRating = "BAD";
	else if (result.totalC2Sectors == 0) result.qualityRating = "EXCELLENT";
	else if (result.maxC2InSingleSector >= 100) result.qualityRating = "POOR";
	else if (result.avgC2PerSecond < 1.0 && result.consecutiveErrorSectors < 3
		&& result.maxC2InSingleSector < 50) result.qualityRating = "GOOD";
	else if (result.avgC2PerSecond < 10.0 && result.consecutiveErrorSectors < 10) result.qualityRating = "ACCEPTABLE";
	else result.qualityRating = "POOR";

	std::cout << "\n" << std::string(60, '=') << "\n";
	std::cout << "              BLER QUALITY SCAN REPORT\n";
	std::cout << std::string(60, '=') << "\n";

	std::cout << "\n--- Scan Configuration ---\n";
	std::cout << "  Speed:           " << (scanSpeed == 0 ? "Max" : std::to_string(scanSpeed) + "x") << "\n";
	std::cout << "  Sectors scanned: " << scannedSectors << "\n";
	std::cout << "  Disc length:     "
		<< (result.totalSeconds / 60) << ":" << std::setfill('0') << std::setw(2) << (result.totalSeconds % 60)
		<< std::setfill(' ') << " (mm:ss)\n";
	if (hasC1Support)
		std::cout << "  C1 reporting:    Yes (block error bytes 294-295)\n";

	// ── C1 Error Report ──
	if (hasC1Support) {
		std::cout << "\n--- C1 Error Statistics ---\n";

		bool c1BlerPass = result.avgC1PerSecond < 220.0;
		std::cout << "  Total C1 errors:      " << result.totalC1Errors << "\n";
		std::cout << "  Sectors with C1:      " << result.totalC1Sectors;
		if (result.totalSectors > 0)
			std::cout << " (" << std::fixed << std::setprecision(3)
			<< (result.totalC1Sectors * 100.0 / result.totalSectors) << "%)";
		std::cout << "\n";
		std::cout << "  Avg C1/sec:           " << std::fixed << std::setprecision(2) << result.avgC1PerSecond;
		std::cout << (c1BlerPass ? "  [PASS]" : "  [FAIL]") << "  (Red Book limit: 220/sec)\n";
		std::cout << "  Max C1/sec:           " << result.maxC1PerSecond << "\n";
		std::cout << "  Max C1 in one sector: " << result.maxC1InSingleSector;
		if (result.maxC1InSingleSector > 0) std::cout << "  (LBA " << result.worstC1SectorLBA << ")";
		std::cout << "\n";

		// C1 quality assessment
		if (result.avgC1PerSecond < 5.0)
			std::cout << "  C1 Assessment:        EXCELLENT — minimal correction needed\n";
		else if (result.avgC1PerSecond < 50.0)
			std::cout << "  C1 Assessment:        GOOD — normal wear\n";
		else if (result.avgC1PerSecond < 220.0)
			std::cout << "  C1 Assessment:        FAIR — elevated but within Red Book limits\n";
		else
			std::cout << "  C1 Assessment:        POOR — exceeds Red Book BLER limit\n";
	}

	std::cout << "\n--- Red Book Compliance ---\n";
	bool blerPass = result.avgC2PerSecond < 220.0;
	std::cout << "  Avg C2/sec:       " << std::fixed << std::setprecision(2) << result.avgC2PerSecond;
	std::cout << (blerPass ? "  [PASS]" : "  [FAIL]") << "  (limit: 220/sec)\n";
	std::cout << "  Max C2/sec:       " << result.maxC2PerSecond;
	if (result.maxC2PerSecond > 0) {
		int worstMin = (result.worstSecondLBA / 75) / 60;
		int worstSec = (result.worstSecondLBA / 75) % 60;
		std::cout << "  at " << worstMin << ":" << std::setfill('0') << std::setw(2) << worstSec << std::setfill(' ');
	}
	std::cout << "\n";

	std::cout << "\n--- Error Statistics ---\n";
	std::cout << "  Total C2 errors:      " << result.totalC2Errors << "\n";
	std::cout << "  Sectors with C2:      " << result.totalC2Sectors;
	if (result.totalSectors > 0)
		std::cout << " (" << std::fixed << std::setprecision(3)
		<< (result.totalC2Sectors * 100.0 / result.totalSectors) << "%)";
	std::cout << "\n";
	std::cout << "  Read failures:        " << result.totalReadFailures << "\n";
	std::cout << "  Max C2 in one sector: " << result.maxC2InSingleSector;
	if (result.maxC2InSingleSector > 0) std::cout << "  (LBA " << result.worstSectorLBA << ")";
	std::cout << "\n";
	std::cout << "  Longest error run:    " << result.consecutiveErrorSectors << " sectors\n";

	std::cout << "\n--- Zone Error Distribution ---\n";
	std::cout << "  Inner  (0-33%):   " << std::fixed << std::setprecision(2)
		<< result.zoneStats.InnerErrorRate() << "% ("
		<< result.zoneStats.innerErrors << "/" << result.zoneStats.innerSectors << ")\n";
	std::cout << "  Middle (33-66%):  " << std::fixed << std::setprecision(2)
		<< result.zoneStats.MiddleErrorRate() << "% ("
		<< result.zoneStats.middleErrors << "/" << result.zoneStats.middleSectors << ")\n";
	std::cout << "  Outer  (66-100%): " << std::fixed << std::setprecision(2)
		<< result.zoneStats.OuterErrorRate() << "% ("
		<< result.zoneStats.outerErrors << "/" << result.zoneStats.outerSectors << ")\n";
	if (result.hasEdgeConcentration)
		std::cout << "  ** Edge concentration detected **\n";
	if (result.hasProgressivePattern)
		std::cout << "  ** Progressive degradation pattern detected **\n";

	if (!result.errorClusters.empty()) {
		std::cout << "\n--- Error Clusters ---\n";
		std::cout << "  Cluster count:    " << result.errorClusters.size() << "\n";
		std::cout << "  Largest cluster:  " << result.largestClusterSize << " sectors\n";
	}

	std::cout << "\n--- Per-Track Summary ---\n";
	if (hasC1Support)
		std::cout << "  Track  Length     C1 Errors  C2 Errors  Seconds  Avg C1/s  Avg C2/s  Status\n";
	else
		std::cout << "  Track  Length     C2 Errors  Seconds  Avg/sec   Status\n";
	std::cout << "  " << std::string(hasC1Support ? 78 : 58, '-') << "\n";
	for (const auto& t : disc.tracks) {
		if (!t.isAudio) continue;
		DWORD tStart = (t.trackNumber == 1) ? 0 : t.pregapLBA;
		DWORD tEnd = t.endLBA;
		DWORD tSectors = tEnd - tStart + 1;
		DWORD tSeconds = (tSectors + 74) / 75;

		int trackC1 = 0;
		int trackC2 = 0;
		int trackC2Seconds = 0;
		for (size_t i = 0; i < result.perSecondC2.size(); i++) {
			DWORD secLBA = static_cast<DWORD>(result.perSecondC2[i].first);
			if (secLBA >= tStart && secLBA <= tEnd) {
				if (result.perSecondC2[i].second > 0) {
					trackC2 += result.perSecondC2[i].second;
					trackC2Seconds++;
				}
				if (hasC1Support && i < result.perSecondC1.size()) {
					trackC1 += result.perSecondC1[i].second;
				}
			}
		}

		double trackAvgC2 = tSeconds > 0 ? static_cast<double>(trackC2) / tSeconds : 0;
		double trackAvgC1 = tSeconds > 0 ? static_cast<double>(trackC1) / tSeconds : 0;
		int trackMin = tSeconds / 60;
		int trackSec = tSeconds % 60;

		const char* status = "Perfect";
		if (trackC2 > 100) status = "Poor";
		else if (trackC2 > 20) status = "Fair";
		else if (trackC2 > 0) status = "Good";
		else if (hasC1Support && trackC1 > 1000) status = "Fair";

		if (hasC1Support) {
			std::cout << "  " << std::setw(3) << t.trackNumber << "    "
				<< trackMin << ":" << std::setfill('0') << std::setw(2) << trackSec << std::setfill(' ') << "   "
				<< std::setw(7) << trackC1 << "     "
				<< std::setw(7) << trackC2 << "     "
				<< std::setw(4) << trackC2Seconds << "     "
				<< std::fixed << std::setprecision(1) << std::setw(6) << trackAvgC1 << "    "
				<< std::fixed << std::setprecision(1) << std::setw(6) << trackAvgC2 << "  "
				<< status << "\n";
		}
		else {
			std::cout << "  " << std::setw(3) << t.trackNumber << "    "
				<< trackMin << ":" << std::setfill('0') << std::setw(2) << trackSec << std::setfill(' ') << "   "
				<< std::setw(7) << trackC2 << "     "
				<< std::setw(4) << trackC2Seconds << "     "
				<< std::fixed << std::setprecision(1) << std::setw(6) << trackAvgC2 << "  "
				<< status << "\n";
		}
	}

	PrintBlerGraph(result);

	std::cout << "\n" << std::string(60, '-') << "\n";
	std::cout << "  QUALITY: " << result.qualityRating << "\n";

	if (result.qualityRating == "EXCELLENT") {
		std::cout << "  No C2 errors. Disc is in excellent condition.\n";
	}
	else if (result.qualityRating == "GOOD") {
		std::cout << "  Minor errors within acceptable limits. Disc is safe to rip.\n";
	}
	else if (result.qualityRating == "ACCEPTABLE") {
		std::cout << "  Moderate error rate. Recommend secure rip mode.\n";
	}
	else if (result.qualityRating == "POOR") {
		std::cout << "  High error rate. Use Paranoid rip mode. Consider cleaning disc.\n";
	}
	else {
		std::cout << "  Read failures detected. Some data may be unrecoverable.\n";
	}
	std::cout << std::string(60, '=') << "\n";

	return true;
}

void AudioCDCopier::PrintBlerGraph(const BlerResult& result, int width, int height) {
	if (result.perSecondC2.empty() || width <= 0 || height <= 0) return;

	// ── C1 Error Distribution Graph ──
	if (result.hasC1Data && !result.perSecondC1.empty()) {
		std::cout << "\n=== C1 Error Distribution ===\n";
		std::cout << "  Each column = a time slice of the disc; height = C1 error count\n\n";

		int maxC1 = 1;
		for (const auto& p : result.perSecondC1) {
			if (p.second > maxC1) maxC1 = p.second;
		}

		std::vector<int> c1Buckets(width, 0);
		size_t c1DataSize = result.perSecondC1.size();
		for (size_t i = 0; i < c1DataSize; i++) {
			size_t bucket = (i * static_cast<size_t>(width)) / c1DataSize;
			if (bucket >= static_cast<size_t>(width)) bucket = static_cast<size_t>(width) - 1;
			c1Buckets[bucket] = std::max(c1Buckets[bucket], result.perSecondC1[i].second);
		}

		int labelWidth = std::max(4, static_cast<int>(std::to_string(maxC1).length()) + 1);

		for (int row = height; row > 0; row--) {
			int threshold = std::max(1, (maxC1 * row) / height);

			if (row == height)
				std::cout << std::setw(labelWidth) << maxC1 << " |";
			else if (row == (height + 1) / 2)
				std::cout << std::setw(labelWidth) << (maxC1 / 2) << " |";
			else if (row == 1)
				std::cout << std::setw(labelWidth) << 0 << " |";
			else
				std::cout << std::string(labelWidth, ' ') << " |";

			for (int col = 0; col < width; col++) {
				if (c1Buckets[col] >= threshold) {
					double severity = static_cast<double>(c1Buckets[col]) / maxC1;
					if (severity > 0.66) std::cout << '#';
					else if (severity > 0.33) std::cout << '+';
					else std::cout << '.';
				}
				else {
					std::cout << ' ';
				}
			}
			std::cout << "\n";
		}

		std::cout << std::string(labelWidth, ' ') << " +" << std::string(width, '-') << "\n";

		int padding = labelWidth + 2;
		int endMin = result.totalSeconds / 60;
		int endSec = result.totalSeconds % 60;
		std::string endStr = std::to_string(endMin) + ":"
			+ (endSec < 10 ? "0" : "") + std::to_string(endSec);

		std::cout << std::string(padding, ' ') << "0:00";
		int gap = width - 4 - static_cast<int>(endStr.length());
		if (gap > 0) std::cout << std::string(gap, ' ');
		std::cout << endStr << "\n";

		std::cout << std::string(padding, ' ')
			<< "# = high (>66%)  + = moderate (33-66%)  . = low (<33%)\n";

		// Red Book BLER limit reference line
		if (maxC1 > 220) {
			std::cout << std::string(padding, ' ')
				<< "!! Peak C1/sec (" << maxC1 << ") exceeds Red Book BLER limit (220/sec)\n";
   		}
	}

	// ── C2 Error Distribution Graph (existing) ──
	std::cout << "\n=== C2 Error Distribution ===\n";
	std::cout << "  Each column = a time slice of the disc; height = C2 error count\n\n";

	int maxC2 = 1;
	for (const auto& p : result.perSecondC2) {
		if (p.second > maxC2) maxC2 = p.second;
	}

	std::vector<int> buckets(width, 0);
	size_t dataSize = result.perSecondC2.size();
	for (size_t i = 0; i < dataSize; i++) {
		size_t bucket = (i * static_cast<size_t>(width)) / dataSize;
		if (bucket >= static_cast<size_t>(width)) bucket = static_cast<size_t>(width) - 1;
		buckets[bucket] = std::max(buckets[bucket], result.perSecondC2[i].second);
	}

	int labelWidth = std::max(4, static_cast<int>(std::to_string(maxC2).length()) + 1);

	for (int row = height; row > 0; row--) {
		int threshold = (maxC2 * row) / height;

		// Y-axis labels at top, middle, and bottom rows
		if (row == height)
			std::cout << std::setw(labelWidth) << maxC2 << " |";
		else if (row == (height + 1) / 2)
			std::cout << std::setw(labelWidth) << (maxC2 / 2) << " |";
		else if (row == 1)
			std::cout << std::setw(labelWidth) << 0 << " |";
		else
			std::cout << std::string(labelWidth, ' ') << " |";

		for (int col = 0; col < width; col++) {
			if (buckets[col] >= threshold) {
				double severity = static_cast<double>(buckets[col]) / maxC2;
				if (severity > 0.66) std::cout << '#';
				else if (severity > 0.33) std::cout << '+';
				else std::cout << '.';
			}
			else {
				std::cout << ' ';
			}
		}
		std::cout << "\n";
	}

	// X-axis line
	std::cout << std::string(labelWidth, ' ') << " +" << std::string(width, '-') << "\n";

	// Time labels along X-axis
	int padding = labelWidth + 2;
	int endMin = result.totalSeconds / 60;
	int endSec = result.totalSeconds % 60;
	std::string endStr = std::to_string(endMin) + ":"
		+ (endSec < 10 ? "0" : "") + std::to_string(endSec);

	std::cout << std::string(padding, ' ') << "0:00";
	int gap = width - 4 - static_cast<int>(endStr.length());
	if (gap > 0) std::cout << std::string(gap, ' ');
	std::cout << endStr << "\n";

	// Legend
	std::cout << std::string(padding, ' ')
		<< "# = high (>66%)  + = moderate (33-66%)  . = low (<33%)\n";
}