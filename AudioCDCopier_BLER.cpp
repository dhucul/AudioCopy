#define NOMINMAX
#include "AudioCDCopier.h"
#include "InterruptHandler.h"
#include <iostream>
#include <iomanip>
#include <vector>
#include <algorithm>
#include <cstring>
#include <cmath>

// ============================================================================
// BLER Quality Scanning
// ============================================================================

bool AudioCDCopier::RunBlerScan(const DiscInfo& disc, BlerResult& result, int scanSpeed) {
	// Check capability first to determine scan title
	bool hasC1Support = m_drive.SupportsC1BlockErrors();

	if (hasC1Support)
		std::cout << "\n=== CD BLER Quality Scan ===\n";
	else
		std::cout << "\n=== CD Integrity Scan (C2 Only) ===\n";

	if (!m_drive.CheckC2Support()) {
		std::cout << "ERROR: C2 not supported.\n";
		return false;
	}

	if (hasC1Support) {
		std::cout << "C1 block error reporting available — C1 and C2 errors will be reported.\n\n";
	}
	else {
		std::cout << "Note: C1 errors are not available on this drive.\n";
		std::cout << "      This scan verifies read integrity (C2) but cannot measure physical disc degradation.\n\n";
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
	progress.SetLabel("  Scanning");
	progress.Start();

	ScsiDrive::C2ReadOptions c2Opts;
	c2Opts.multiPass = false;
	c2Opts.countBytes = false;  // Bit counting — standard C2 error pointer interpretation
	c2Opts.defeatCache = true;

	std::vector<BYTE> c2Buffer(C2_ERROR_SIZE, 0);

	// Collects (LBA, C2 count) for all sectors with C2 errors — trimmed after the loop
	std::vector<std::pair<DWORD, int>> sectorErrors;

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

				// Use the higher of the two C2 measurements — the error pointer
				// bitmap (c2Errors) and the CIRC block count (c2BlockErrors) report
				// the same errors by different methods.  Taking the max prevents
				// under-reporting when one method returns 0 while the other detects
				// errors.  When hasC1Support is false, c2BlockErrors stays 0 so
				// effectiveC2 == c2Errors and behaviour is unchanged.
				int effectiveC2 = std::max(c2Errors, c2BlockErrors);

				if (effectiveC2 > 0) {
					if (!recovered) {
						result.totalC2Errors += effectiveC2;
						result.totalC2Sectors++;
						result.perSecondC2[secIdx].second += effectiveC2;

						if (effectiveC2 > result.maxC2InSingleSector) {
							result.maxC2InSingleSector = effectiveC2;
							result.worstSectorLBA = lba;
						}

						zoneError = 1;
						errorLBAs.push_back(lba);
						sectorErrors.push_back({ lba, effectiveC2 });

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

	// Sort and keep top 10 worst C2 sectors by error count
	std::sort(sectorErrors.begin(), sectorErrors.end(),
		[](const auto& a, const auto& b) { return a.second > b.second; });
	if (sectorErrors.size() > 10) sectorErrors.resize(10);
	result.topWorstC2Sectors = std::move(sectorErrors);

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
				result.worstC1SecondLBA = static_cast<DWORD>(result.perSecondC1[i].first);
			}
		}

		// C2 proximity: express C1 rate as a fraction of the Red Book 220/sec limit.
		// The margin score is how much of the correction budget is still unused.
		result.c1UtilizationPct = std::min(100.0, result.avgC1PerSecond / 220.0 * 100.0);
		result.peakC1UtilizationPct = std::min(100.0, result.maxC1PerSecond / 220.0 * 100.0);
		double util = result.avgC1PerSecond / 220.0;
		// Use lround to avoid truncation (e.g. 99.54 → 100 rather than 99)
		result.c2MarginScore = static_cast<int>(
			std::lround(std::max(0.0, (1.0 - util) * 100.0)));

		// Override: if C2 errors already exist, the margin is exhausted regardless
		// of what C1 says — clamp the score to reflect actual disc state.
		if (result.totalC2Sectors > 0) {
			result.c2MarginScore = 0;
			result.c2MarginLabel = "EXHAUSTED";
		}
		else if (result.avgC1PerSecond < 50.0)  result.c2MarginLabel = "WIDE";
		else if (result.avgC1PerSecond < 150.0) result.c2MarginLabel = "ADEQUATE";
		else if (result.avgC1PerSecond < 220.0) result.c2MarginLabel = "NARROW";
		else                                     result.c2MarginLabel = "CRITICAL";
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

	if (result.totalReadFailures > 0) {
		result.qualityRating = "BAD";
	}
	else if (result.totalC2Sectors == 0) {
		// No C2 errors — grade by C1 margin when available, otherwise assume best.
		if (!hasC1Support || result.avgC1PerSecond < 50.0)
			result.qualityRating = "EXCELLENT";
		else if (result.avgC1PerSecond < 150.0)
			result.qualityRating = "GOOD";       // no C2, but narrowing margin
		else if (result.avgC1PerSecond < 220.0)
			result.qualityRating = "ACCEPTABLE"; // no C2, but very narrow margin
		else
			result.qualityRating = "FAIR";       // C1 exceeds Red Book, no C2 yet
	}
	else {
		// C2 errors present
		if (result.avgC2PerSecond < 1.0 && result.consecutiveErrorSectors < 3
			&& result.maxC2InSingleSector < 50) result.qualityRating = "GOOD";
		else if (result.avgC2PerSecond < 10.0 && result.consecutiveErrorSectors < 10
			&& result.maxC2InSingleSector < 100) result.qualityRating = "ACCEPTABLE";
		else if (result.avgC2PerSecond < 50.0) result.qualityRating = "FAIR";
		else result.qualityRating = "POOR";
	}

	std::cout << "\n" << std::string(60, '=') << "\n";
	if (hasC1Support)
		std::cout << "              BLER QUALITY SCAN REPORT\n";
	else
		std::cout << "              DISC INTEGRITY REPORT (C2)\n";
	std::cout << std::string(60, '=') << "\n";

	std::cout << "\n--- Scan Configuration ---\n";
	std::cout << "  Speed:           " << (scanSpeed == 0 ? "Max" : std::to_string(scanSpeed) + "x") << "\n";
	std::cout << "  Sectors scanned: " << scannedSectors << "\n";
	std::cout << "  Disc length:     "
		<< (result.totalSeconds / 60) << ":" << std::setfill('0') << std::setw(2) << (result.totalSeconds % 60)
		<< std::setfill(' ') << " (mm:ss)\n";
	if (hasC1Support)
		std::cout << "  C1 reporting:    Yes (block error bytes 294-295)\n";
	else
		std::cout << "  C1 reporting:    No (drive does not support C1 stats)\n";

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
		std::cout << "  Max C1/sec:           " << result.maxC1PerSecond;
		if (result.maxC1PerSecond > 0) {
			int worstMin = (result.worstC1SecondLBA / 75) / 60;
			int worstSec = (result.worstC1SecondLBA / 75) % 60;
			std::cout << "  at " << worstMin << ":" << std::setfill('0') << std::setw(2) << worstSec << std::setfill(' ');
		}
		std::cout << "\n";
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

	std::cout << "\n--- C2 Error Statistics ---\n";
	std::cout << "  Avg C2/sec:       " << std::fixed << std::setprecision(2) << result.avgC2PerSecond;
	if (result.avgC2PerSecond == 0.0)
		std::cout << "  [PASS]  (no uncorrectable errors)\n";
	else if (result.avgC2PerSecond < 1.0)
		std::cout << "  [WARN]  (minor uncorrectable errors)\n";
	else
		std::cout << "  [FAIL]  (significant uncorrectable errors)\n";
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

		// Show top 10 clusters sorted by size descending
		auto sortedClusters = result.errorClusters;
		std::sort(sortedClusters.begin(), sortedClusters.end(),
			[](const ErrorCluster& a, const ErrorCluster& b) { return a.size() > b.size(); });
		int clusterShow = std::min(10, static_cast<int>(sortedClusters.size()));
		std::cout << "\n  Top " << clusterShow << " cluster" << (clusterShow > 1 ? "s" : "") << ":\n";
		std::cout << "  #    Start LBA    End LBA     Size   Position\n";
		std::cout << "  " << std::string(50, '-') << "\n";
		for (int i = 0; i < clusterShow; i++) {
			const auto& c = sortedClusters[i];
			int posMin = (c.startLBA / 75) / 60;
			int posSec = (c.startLBA / 75) % 60;
			std::cout << "  " << std::setw(2) << (i + 1) << "   "
				<< std::setw(10) << c.startLBA << "   "
				<< std::setw(10) << c.endLBA << "   "
				<< std::setw(4) << c.size() << "   "
				<< posMin << ":" << std::setfill('0') << std::setw(2) << posSec
				<< std::setfill(' ') << "\n";
		}
	}

	if (!result.topWorstC2Sectors.empty()) {
		std::cout << "\n--- Top " << result.topWorstC2Sectors.size() << " Worst C2 Sectors ---\n";
		if (result.totalReadFailures > 0)
			std::cout << "  Note: " << result.totalReadFailures
			<< " read-failure sector(s) are not listed here (no C2 count available).\n";
		std::cout << "  #    LBA         C2 Count   Position\n";
		std::cout << "  " << std::string(44, '-') << "\n";
		for (size_t i = 0; i < result.topWorstC2Sectors.size(); i++) {
			auto [lba, cnt] = result.topWorstC2Sectors[i];
			int posMin = (lba / 75) / 60;
			int posSec = (lba / 75) % 60;
			std::cout << "  " << std::setw(2) << (i + 1) << "   "
				<< std::setw(9) << lba << "   "
				<< std::setw(8) << cnt << "   "
				<< posMin << ":" << std::setfill('0') << std::setw(2) << posSec
				<< std::setfill(' ') << "\n";
		}
	}

	{
		std::cout << "\n--- C2 Error Density Distribution ---\n";
		int tier0 = 0, tier1 = 0, tier2 = 0, tier3 = 0, tier4 = 0, tier5 = 0;
		// Loop only over actual seconds, not the +1 overflow guard slot
		int totalSec = static_cast<int>(result.totalSeconds);
		for (size_t i = 0; i < result.totalSeconds; i++) {
			int v = result.perSecondC2[i].second;
			if (v == 0)        tier0++;
			else if (v <= 5)   tier1++;
			else if (v <= 20)  tier2++;
			else if (v <= 50)  tier3++;
			else if (v <= 100) tier4++;
			else               tier5++;
		}
		auto pct = [&](int n) -> double {
			return totalSec > 0 ? n * 100.0 / totalSec : 0.0;
			};
		std::cout << std::fixed << std::setprecision(1);
		std::cout << "  0 errors:      " << std::setw(5) << tier0 << " sec  (" << pct(tier0) << "%)\n";
		std::cout << "  1-5 errors:    " << std::setw(5) << tier1 << " sec  (" << pct(tier1) << "%)\n";
		std::cout << "  6-20 errors:   " << std::setw(5) << tier2 << " sec  (" << pct(tier2) << "%)\n";
		std::cout << "  21-50 errors:  " << std::setw(5) << tier3 << " sec  (" << pct(tier3) << "%)\n";
		std::cout << "  51-100 errors: " << std::setw(5) << tier4 << " sec  (" << pct(tier4) << "%)\n";
		std::cout << "  100+ errors:   " << std::setw(5) << tier5 << " sec  (" << pct(tier5) << "%)\n";
	}

	std::cout << "\n--- Per-Track Summary ---\n";
	if (hasC1Support)
		std::cout << "  Track  Length     C1 Errors  C2 Errors  Err Secs  Avg C1/s  Avg C2/s  Status\n";
	else
		std::cout << "  Track  Length     C2 Errors  Err Secs  Avg/sec   Status\n";
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
		int trackReadFailures = 0;
		for (size_t i = 0; i < result.perSecondC2.size(); i++) {
			DWORD secLBA = static_cast<DWORD>(result.perSecondC2[i].first);
			if (secLBA >= tStart && secLBA <= tEnd) {
				if (result.perSecondC2[i].second > 0) {
					trackC2 += result.perSecondC2[i].second;
					trackC2Seconds++;
				}
				if (hasC1Support && i < result.perSecondC1.size()
					&& result.perSecondC1[i].second > 0) {
					trackC1 += result.perSecondC1[i].second;
				}
			}
		}

		// Count read failures for this track from the error cluster list
		for (const auto& cluster : result.errorClusters) {
			// Overlap between cluster [cluster.startLBA, cluster.endLBA] and track [tStart, tEnd]
			if (cluster.endLBA >= tStart && cluster.startLBA <= tEnd) {
				DWORD overlapStart = (cluster.startLBA > tStart) ? cluster.startLBA : tStart;
				DWORD overlapEnd = (cluster.endLBA < tEnd) ? cluster.endLBA : tEnd;
				trackReadFailures += static_cast<int>(overlapEnd - overlapStart + 1);
			}
		}

		double trackAvgC2 = tSeconds > 0 ? static_cast<double>(trackC2) / tSeconds : 0;
		double trackAvgC1 = tSeconds > 0 ? static_cast<double>(trackC1) / tSeconds : 0;
		int trackMin = tSeconds / 60;
		int trackSec = tSeconds % 60;

		const char* status = "Perfect";
		if (trackReadFailures > 0) status = "BAD";
		else if (trackC2 > 100) status = "Poor";
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

	// ── C2 Proximity / Margin Analysis ──
	std::cout << "\n--- C2 Error Proximity ---\n";
	if (hasC1Support) {
		std::cout << "  Avg C1 budget used:  " << std::fixed << std::setprecision(1)
			<< result.avgC1PerSecond << " / 220.0 /sec  ("
			<< result.c1UtilizationPct << "% of Red Book limit)\n";
		std::cout << "  Peak C1 budget used: " << result.maxC1PerSecond << " / 220.0 /sec  ("
			<< std::setprecision(1) << result.peakC1UtilizationPct << "% of limit)\n";
		std::cout << "  C2 margin score:     " << result.c2MarginScore
			<< " / 100  [" << result.c2MarginLabel << "]\n";

		constexpr int kBarWidth = 38;
		int filled = (result.c2MarginScore * kBarWidth) / 100;
		std::cout << "  Headroom: [";
		for (int i = 0; i < kBarWidth; i++)
			std::cout << (i < filled ? '=' : ' ');
		std::cout << "] " << result.c2MarginScore << "%\n";

		if (result.c2MarginLabel == "EXHAUSTED")
			std::cout << "  !! C2 errors already present — correction margin is exceeded.\n";
		else if (result.c2MarginLabel == "WIDE")
			std::cout << "  Strong correction headroom — disc is healthy.\n";
		else if (result.c2MarginLabel == "ADEQUATE")
			std::cout << "  Normal wear. Comfortable headroom for reliable ripping.\n";
		else if (result.c2MarginLabel == "NARROW")
			std::cout << "  Elevated C1 rate. Recommend ripping at 4-8x.\n";
		else
			std::cout << "  !! C1 near Red Book limit. C2 errors likely on re-reads or at higher speed.\n";
	}
	else {
		std::cout << "  C1 data unavailable — C2 proximity/margin cannot be determined.\n";
		std::cout << "  This drive verifies sectors are readable but cannot measure signal quality.\n";
	}

	std::cout << "\n" << std::string(60, '-') << "\n";
	std::cout << "  QUALITY: " << result.qualityRating << "\n";

	if (result.qualityRating == "EXCELLENT") {
		if (hasC1Support)
			std::cout << "  No C2 errors. C1 rate low — disc has strong error margin.\n";
		else
			std::cout << "  No C2 (uncorrectable) errors detected.\n";
	}
	else if (result.qualityRating == "GOOD") {
		if (result.totalC2Sectors == 0)
			std::cout << "  No C2 errors, but C1 rate is elevated. Margin is narrowing.\n";
		else
			std::cout << "  Minor errors within acceptable limits. Disc is safe to rip.\n";
	}
	else if (result.qualityRating == "ACCEPTABLE") {
		if (result.totalC2Sectors == 0)
			std::cout << "  No C2 errors yet, but C1 rate is high. Rip soon at low speed.\n";
		else
			std::cout << "  Moderate error rate. Recommend secure rip mode.\n";
	}
	else if (result.qualityRating == "FAIR") {
		if (result.totalC2Sectors == 0)
			std::cout << "  C1 rate exceeds Red Book limit. C2 errors may appear under stress.\n";
		else
			std::cout << "  Elevated error rate. Use secure or paranoid rip mode.\n";
	}
	else if (result.qualityRating == "POOR") {
		std::cout << "  High C2 error rate. Use Paranoid rip mode. Consider cleaning disc.\n";
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

	// ── C2 Error Distribution Graph ──
	if (result.totalC2Errors == 0 && result.totalReadFailures == 0) {
		std::cout << "\n=== C2 Error Distribution ===\n";
		std::cout << "  No C2 errors — graph skipped.\n";
	}
	else {
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
			// FIX: Floor threshold at 1 to prevent 0 >= 0 rendering empty
			// buckets as dots.  This matches the C1 graph logic.
			int threshold = std::max(1, (maxC2 * row) / height);

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
}