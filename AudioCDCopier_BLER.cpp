#define NOMINMAX
#include "AudioCDCopier.h"
#include "InterruptHandler.h"
#include <iostream>
#include <iomanip>
#include <vector>
#include <algorithm>

// ============================================================================
// BLER Quality Scanning
// ============================================================================

bool AudioCDCopier::RunBlerScan(const DiscInfo& disc, BlerResult& result, int scanSpeed) {
	std::cout << "\n=== CD BLER Quality Scan ===\n";
	std::cout << "Note: C1 errors are not accessible via standard SCSI commands.\n";
	std::cout << "      This scan reports C2 (uncorrectable) errors only.\n\n";

	if (!m_drive.CheckC2Support()) {
		std::cout << "ERROR: C2 not supported.\n";
		return false;
	}

	DWORD totalSectors = 0, firstLBA = 0;
	bool first = true;
	for (const auto& t : disc.tracks) {
		if (t.isAudio) {
			DWORD start = (t.trackNumber == 1) ? 0 : t.pregapLBA;
			if (first) { firstLBA = start; first = false; }
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

	std::cout << "Scanning " << totalSectors << " sectors...\n";
	std::cout << "  (Press ESC or Ctrl+C to cancel)\n\n";
	m_drive.SetSpeed(scanSpeed);

	DWORD scannedSectors = 0;
	int currentErrorRun = 0;

	ProgressIndicator progress(40);
	progress.SetLabel("  BLER Scan");
	progress.Start();

	ScsiDrive::C2ReadOptions c2Opts;
	c2Opts.multiPass = false;
	c2Opts.countBytes = true;

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

			// Use audio-relative sector count for indexing to handle
			// mixed-mode discs with non-audio gaps between tracks
			size_t secIdx = static_cast<size_t>(scannedSectors / 75);
			if (secIdx >= result.perSecondC2.size())
				secIdx = result.perSecondC2.size() - 1;

			// Record the starting LBA for each time bucket
			if (scannedSectors % 75 == 0)
				result.perSecondC2[secIdx].first = static_cast<int>(lba);

			if (m_drive.ReadSectorWithC2Ex(lba, buf.data(), nullptr, c2Errors, nullptr, c2Opts)) {
				if (c2Errors > 0) {
					result.totalC2Errors += c2Errors;
					result.totalC2Sectors++;
					result.perSecondC2[secIdx].second += c2Errors;

					if (c2Errors > result.maxC2InSingleSector) {
						result.maxC2InSingleSector = c2Errors;
						result.worstSectorLBA = lba;
					}

					currentErrorRun++;
					if (currentErrorRun > result.consecutiveErrorSectors) {
						result.consecutiveErrorSectors = currentErrorRun;
					}
				}
				else {
					currentErrorRun = 0;
				}
			}
			else {
				result.totalReadFailures++;
				result.perSecondC2[secIdx].second += C2_ERROR_SIZE;
				currentErrorRun++;
				if (currentErrorRun > result.consecutiveErrorSectors) {
					result.consecutiveErrorSectors = currentErrorRun;
				}
			}

			scannedSectors++;
			progress.Update(static_cast<int>(scannedSectors), static_cast<int>(totalSectors));
		}
	}

	progress.Finish(true);
	m_drive.SetSpeed(0);

	result.avgC2PerSecond = result.totalSeconds > 0
		? static_cast<double>(result.totalC2Errors) / result.totalSeconds : 0;

	for (size_t i = 0; i < result.perSecondC2.size(); i++) {
		if (result.perSecondC2[i].second > result.maxC2PerSecond) {
			result.maxC2PerSecond = result.perSecondC2[i].second;
			result.worstSecondLBA = result.perSecondC2[i].first;
		}
	}

	if (result.totalReadFailures > 0) result.qualityRating = "BAD";
	else if (result.totalC2Sectors == 0) result.qualityRating = "EXCELLENT";
	else if (result.avgC2PerSecond < 1.0 && result.consecutiveErrorSectors < 3) result.qualityRating = "GOOD";
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

	std::cout << "\n--- C2 Error Assessment ---\n";
	bool c2Pass = result.avgC2PerSecond < 1.0;
	std::cout << "  Avg C2/sec:       " << std::fixed << std::setprecision(2) << result.avgC2PerSecond;
	std::cout << (c2Pass ? "  [PASS]" : "  [FAIL]") << "  (limit: <1.0/sec for good quality)\n";
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

	std::cout << "\n--- Per-Track Summary ---\n";
	std::cout << "  Track  Length     C2 Errors  Sectors  Avg/sec   Status\n";
	std::cout << "  " << std::string(58, '-') << "\n";
	for (const auto& t : disc.tracks) {
		if (!t.isAudio) continue;
		DWORD tStart = (t.trackNumber == 1) ? 0 : t.pregapLBA;
		DWORD tEnd = t.endLBA;
		DWORD tSectors = tEnd - tStart + 1;
		DWORD tSeconds = (tSectors + 74) / 75;

		int trackC2 = 0;
		int trackC2Sectors = 0;
		for (size_t i = 0; i < result.perSecondC2.size(); i++) {
			DWORD secLBA = static_cast<DWORD>(result.perSecondC2[i].first);
			if (secLBA >= tStart && secLBA <= tEnd && result.perSecondC2[i].second > 0) {
				trackC2 += result.perSecondC2[i].second;
				trackC2Sectors++;
			}
		}

		double trackAvg = tSeconds > 0 ? static_cast<double>(trackC2) / tSeconds : 0;
		int trackMin = tSeconds / 60;
		int trackSec = tSeconds % 60;

		const char* status = "Perfect";
		if (trackC2 > 100) status = "Poor";
		else if (trackC2 > 20) status = "Fair";
		else if (trackC2 > 0) status = "Good";

		std::cout << "  " << std::setw(3) << t.trackNumber << "    "
			<< trackMin << ":" << std::setfill('0') << std::setw(2) << trackSec << std::setfill(' ') << "   "
			<< std::setw(7) << trackC2 << "     "
			<< std::setw(4) << trackC2Sectors << "     "
			<< std::fixed << std::setprecision(1) << std::setw(6) << trackAvg << "  "
			<< status << "\n";
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