#define NOMINMAX
#include "AudioCDCopier.h"
#include "InterruptHandler.h"
#include <iostream>
#include <iomanip>
#include <vector>
#include <algorithm>
#include <cstring>

// ============================================================================
// Disc Rot Detection
// ============================================================================

bool AudioCDCopier::RunDiscRotScan(DiscInfo& disc, DiscRotAnalysis& result, int scanSpeed) {
	std::cout << "\n=== Disc Rot Detection Scan ===\n";
	std::cout << "This scan checks for physical disc degradation patterns.\n\n";

	EnsureCapabilitiesDetected();

	if (!m_drive.CheckC2Support()) {
		std::cout << "ERROR: C2 error detection required but not supported.\n";
		return false;
	}

	DWORD firstLBA = 0, lastLBA = 0;
	DWORD totalSectors = 0;
	for (const auto& t : disc.tracks) {
		if (t.isAudio) {
			DWORD start = (t.trackNumber == 1) ? 0 : t.pregapLBA;
			if (totalSectors == 0) firstLBA = start;
			lastLBA = t.endLBA;
			totalSectors += t.endLBA - start + 1;
		}
	}

	if (totalSectors == 0) {
		std::cout << "No audio tracks to scan.\n";
		return false;
	}

	result = DiscRotAnalysis{};
	std::vector<DWORD> errorLBAs;
	std::vector<DWORD> inconsistentLBAs;

	std::cout << "Phase 1: C2 error distribution scan...\n";
	m_drive.SetSpeed(scanSpeed);

	ProgressIndicator progress(40);
	progress.SetLabel("  C2 Scan");
	progress.Start();

	ScsiDrive::C2ReadOptions c2Opts;
	c2Opts.countBytes = true;

	DWORD scannedSectors = 0;
	int maxC2InSector = 0;
	for (const auto& t : disc.tracks) {
		if (!t.isAudio) continue;
		DWORD start = (t.trackNumber == 1) ? 0 : t.pregapLBA;

		for (DWORD lba = start; lba <= t.endLBA; lba++) {
			if (g_interrupt.IsInterrupted() || g_interrupt.CheckEscapeKey()) {
				m_drive.SetSpeed(0);
				return false;
			}

			std::vector<BYTE> buf(AUDIO_SECTOR_SIZE);
			int c2Errors = 0;
			if (m_drive.ReadSectorWithC2Ex(lba, buf.data(), nullptr, c2Errors, nullptr, c2Opts)) {
				ClassifyZone(lba, firstLBA, lastLBA, c2Errors > 0 ? 1 : 0, result.zones);
				if (c2Errors > 0) {
					errorLBAs.push_back(lba);
					if (c2Errors > maxC2InSector)
						maxC2InSector = c2Errors;
				}
			}
			else {
				ClassifyZone(lba, firstLBA, lastLBA, 1, result.zones);
				errorLBAs.push_back(lba);
			}

			scannedSectors++;
			progress.Update(static_cast<int>(scannedSectors), static_cast<int>(totalSectors));
		}
	}
	progress.Finish(true);

	std::sort(errorLBAs.begin(), errorLBAs.end());
	DetectErrorClusters(errorLBAs, result.clusters, scanSpeed);
	result.maxC2InSingleSector = maxC2InSector;

	// Adaptive Zone-Based Sampling
	std::cout << "\nPhase 2: Adaptive read consistency check...\n";

	double innerRate = result.zones.InnerErrorRate();
	double middleRate = result.zones.MiddleErrorRate();
	double outerRate = result.zones.OuterErrorRate();

	auto calcSampleInterval = [](double errorRate) -> int {
		if (errorRate > 2.0) return 20;
		if (errorRate > 0.5) return 50;
		if (errorRate > 0.1) return 100;
		return 200;
		};

	int innerInterval = calcSampleInterval(innerRate);
	int middleInterval = calcSampleInterval(middleRate);
	int outerInterval = calcSampleInterval(outerRate);

	int expectedSamples = 0;
	for (const auto& t : disc.tracks) {
		if (!t.isAudio) continue;
		DWORD start = (t.trackNumber == 1) ? 0 : t.pregapLBA;
		for (DWORD lba = start; lba <= t.endLBA; lba++) {
			DWORD range = lastLBA - firstLBA;
			DWORD pos = lba - firstLBA;
			double pct = range > 0 ? static_cast<double>(pos) / range : 0;
			int sampleInterval = 200;
			if (pct < 0.33) sampleInterval = innerInterval;
			else if (pct < 0.66) sampleInterval = middleInterval;
			else sampleInterval = outerInterval;
			if ((lba - start) % sampleInterval == 0) expectedSamples++;
		}
	}

	int samplesChecked = 0;
	int inconsistentSamples = 0;

	progress.SetLabel("  Adaptive Check");
	progress.Start();

	for (const auto& t : disc.tracks) {
		if (!t.isAudio) continue;
		DWORD start = (t.trackNumber == 1) ? 0 : t.pregapLBA;

		for (DWORD lba = start; lba <= t.endLBA; lba++) {
			if (g_interrupt.IsInterrupted() || g_interrupt.CheckEscapeKey()) {
				break;
			}

			DWORD range = lastLBA - firstLBA;
			DWORD pos = lba - firstLBA;
			double pct = range > 0 ? static_cast<double>(pos) / range : 0;
			int sampleInterval = 200;

			if (pct < 0.33) sampleInterval = innerInterval;
			else if (pct < 0.66) sampleInterval = middleInterval;
			else sampleInterval = outerInterval;

			if ((lba - start) % sampleInterval != 0) continue;

			int inconsistent = 0;
			if (TestReadConsistency(lba, 3, inconsistent, scanSpeed)) {
				samplesChecked++;
				if (inconsistent > 0) {
					inconsistentSamples++;
					inconsistentLBAs.push_back(lba);
				}
			}

			progress.Update(samplesChecked, expectedSamples);
		}
	}
	progress.Finish(true);

	result.totalRereadTests = samplesChecked;
	result.inconsistentSectors = inconsistentSamples;
	result.inconsistencyRate = samplesChecked > 0
		? static_cast<double>(inconsistentSamples) / samplesChecked * 100.0 : 0;

	m_drive.SetSpeed(0);

	AnalyzeErrorPatterns(errorLBAs, result);

	if (result.rotRiskLevel == "NONE") {
		result.recommendation = "Disc appears healthy. Store properly to prevent future damage.";
	}
	else if (result.rotRiskLevel == "LOW") {
		result.recommendation = "Minor issues detected. Consider backing up soon.";
	}
	else if (result.rotRiskLevel == "MODERATE") {
		result.recommendation = "Disc showing early degradation signs. Back up immediately.";
	}
	else if (result.rotRiskLevel == "HIGH") {
		result.recommendation = "Significant degradation detected. Back up NOW - data loss likely.";
	}
	else {
		result.recommendation = "CRITICAL damage! Extract whatever data possible immediately.";
	}

	PrintDiscRotReport(result);
	return true;
}

// ============================================================================
// Helper Functions
// ============================================================================

bool AudioCDCopier::TestReadConsistency(DWORD lba, int passes, int& mismatchCount, int readSpeed) {
	mismatchCount = 0;
	if (passes < 2) return true;

	// Set consistent read speed for all consistency checks to ensure
	// reproducible results; speed variations can affect read stability.
	m_drive.SetSpeed(readSpeed);

	std::vector<BYTE> reference(AUDIO_SECTOR_SIZE);
	if (!m_drive.ReadSectorAudioOnly(lba, reference.data()))
		return false;

	std::vector<BYTE> compare(AUDIO_SECTOR_SIZE);
	for (int i = 1; i < passes; i++) {
		// Without Accurate Stream, the drive may return cached data instead
		// of re-reading from the disc, hiding genuine read inconsistencies
		if (!m_hasAccurateStream) {
			DefeatDriveCache(lba, 0);
		}

		if (!m_drive.ReadSectorAudioOnly(lba, compare.data()))
			return false;
		if (memcmp(reference.data(), compare.data(), AUDIO_SECTOR_SIZE) != 0)
			mismatchCount++;
	}
	return true;
}

void AudioCDCopier::ClassifyZone(DWORD lba, DWORD totalStart, DWORD totalEnd,
	int hasError, DiscZoneStats& zones) {
	DWORD range = totalEnd - totalStart;
	if (range == 0) return;

	DWORD relative = lba - totalStart;
	double position = static_cast<double>(relative) / range;

	if (position < 0.33) {
		zones.innerSectors++;
		zones.innerErrors += hasError;
	}
	else if (position < 0.66) {
		zones.middleSectors++;
		zones.middleErrors += hasError;
	}
	else {
		zones.outerSectors++;
		zones.outerErrors += hasError;
	}
}

int AudioCDCopier::CalculateClusterTolerance(int scanSpeed) {
	// scanSpeed typically ranges from 1-48x
	// Map to tolerance: slower speeds = tighter clusters (lower tolerance)
	//                   faster speeds = scattered sectors (higher tolerance)
	if (scanSpeed >= 40) return 8;      // Very fast: 8-sector window
	if (scanSpeed >= 24) return 7;      // Fast: 7-sector window
	if (scanSpeed >= 16) return 6;      // Medium-fast: 6-sector window
	if (scanSpeed >= 8) return 5;       // Medium: 5-sector window
	if (scanSpeed >= 4) return 4;       // Medium-slow: 4-sector window
	return 3;                           // Slow: 3-sector window (tight clustering)
}

void AudioCDCopier::DetectErrorClusters(const std::vector<DWORD>& errorLBAs,
	std::vector<ErrorCluster>& clusters, int scanSpeed) {
	clusters.clear();
	if (errorLBAs.empty()) return;

	int tolerance = CalculateClusterTolerance(scanSpeed);

	ErrorCluster current;
	current.startLBA = errorLBAs[0];
	current.endLBA = errorLBAs[0];
	current.errorCount = 1;

	for (size_t i = 1; i < errorLBAs.size(); i++) {
		// If the next error is within the adaptive tolerance window, extend the current cluster
		if (errorLBAs[i] <= current.endLBA + tolerance) {
			current.endLBA = errorLBAs[i];
			current.errorCount++;
		}
		else {
			// Gap exceeds tolerance; finalize current cluster and start a new one
			clusters.push_back(current);
			current.startLBA = errorLBAs[i];
			current.endLBA = errorLBAs[i];
			current.errorCount = 1;
		}
	}
	// Don't forget the last cluster
	clusters.push_back(current);
}

void AudioCDCopier::AnalyzeErrorPatterns(const std::vector<DWORD>& errorLBAs,
	DiscRotAnalysis& analysis) {
	if (errorLBAs.empty()) {
		analysis.rotRiskLevel = "NONE";
		return;
	}

	if (analysis.zones.outerSectors > 0) {
		double outerRate = analysis.zones.OuterErrorRate();
		double innerRate = analysis.zones.InnerErrorRate();
		analysis.edgeConcentration = (outerRate > innerRate * 2.0) && (outerRate > 1.0);
	}

	analysis.progressivePattern =
		analysis.zones.InnerErrorRate() < analysis.zones.MiddleErrorRate() &&
		analysis.zones.MiddleErrorRate() < analysis.zones.OuterErrorRate() &&
		analysis.zones.OuterErrorRate() > 0.5;

	int smallClusters = 0;
	for (const auto& c : analysis.clusters) {
		if (c.size() <= 3) smallClusters++;
	}
	analysis.pinholePattern = (smallClusters > 10) &&
		(smallClusters > static_cast<int>(analysis.clusters.size()) / 2);

	analysis.readInstability = analysis.inconsistencyRate > 5.0;

	analysis.rotRiskLevel = AssessRotRisk(analysis);
}

std::string AudioCDCopier::AssessRotRisk(const DiscRotAnalysis& analysis) {
	int score = 0;

	if (analysis.edgeConcentration) score += 25;
	if (analysis.progressivePattern) score += 25;
	if (analysis.pinholePattern) score += 15;
	if (analysis.readInstability) score += 20;
	if (analysis.inconsistencyRate > 10.0) score += 15;

	// Severe C2 errors in a single sector indicate physical damage
	if (analysis.maxC2InSingleSector >= 100) score += 20;
	else if (analysis.maxC2InSingleSector >= 50) score += 10;

	if (score >= 75) return "CRITICAL";
	if (score >= 50) return "HIGH";
	if (score >= 30) return "MODERATE";
	if (score >= 10) return "LOW";
	return "NONE";
}

void AudioCDCopier::PrintDiscRotReport(const DiscRotAnalysis& analysis) {
	std::cout << "\n" << std::string(60, '=') << "\n";
	std::cout << "              DISC ROT ANALYSIS REPORT\n";
	std::cout << std::string(60, '=') << "\n";

	std::cout << "\n--- Zone Error Rates ---\n";
	std::cout << "  (Disc surface divided into three radial zones)\n\n";

	auto printZone = [](const char* label, double rate, int errors, int sectors) {
		int barLen = std::min(20, static_cast<int>(rate * 2));
		std::cout << "  " << label << std::fixed << std::setprecision(2) << rate << "% ("
			<< errors << "/" << sectors << ")  [";
		for (int i = 0; i < 20; i++) std::cout << (i < barLen ? '#' : ' ');
		std::cout << "]\n";
		};

	printZone("Inner  (0-33%):   ", analysis.zones.InnerErrorRate(),
		analysis.zones.innerErrors, analysis.zones.innerSectors);
	printZone("Middle (33-66%):  ", analysis.zones.MiddleErrorRate(),
		analysis.zones.middleErrors, analysis.zones.middleSectors);
	printZone("Outer  (66-100%): ", analysis.zones.OuterErrorRate(),
		analysis.zones.outerErrors, analysis.zones.outerSectors);

	std::cout << "\n--- Error Clusters ---\n";
	std::cout << "  Total clusters:  " << analysis.clusters.size() << "\n";
	if (!analysis.clusters.empty()) {
		int maxSize = 0;
		for (const auto& c : analysis.clusters)
			if (c.size() > maxSize) maxSize = c.size();
		std::cout << "  Largest cluster: " << maxSize << " sectors";
		if (maxSize > 100) std::cout << "  (severe - large contiguous damage)";
		else if (maxSize > 20) std::cout << "  (moderate - localized damage)";
		else std::cout << "  (minor - small scratch or defect)";
		std::cout << "\n";
	}

	std::cout << "\n--- Disc Rot Indicators ---\n";
	auto indicator = [](bool v, const char* yesExplain, const char* noExplain) {
		if (v) std::cout << "YES  - " << yesExplain << "\n";
		else std::cout << "NO   - " << noExplain << "\n";
		};

	std::cout << "  Edge concentration:  ";
	indicator(analysis.edgeConcentration,
		"Errors concentrated at disc edges (classic rot pattern)",
		"Errors not edge-concentrated");
	std::cout << "  Progressive pattern: ";
	indicator(analysis.progressivePattern,
		"Error rate increases toward outer edge (spreading damage)",
		"No progressive error increase");
	std::cout << "  Pinhole pattern:     ";
	indicator(analysis.pinholePattern,
		"Small scattered error spots (early-stage pitting)",
		"No pinhole defects detected");
	std::cout << "  Read instability:    ";
	if (analysis.readInstability)
		std::cout << "YES  - Same sectors return different data on re-read ("
		<< static_cast<int>(analysis.inconsistencyRate) << "% unstable)\n";
	else
		std::cout << "NO   - Reads are consistent across re-reads\n";

	std::cout << "\n--- Risk Assessment ---\n";
	std::cout << "  Disc Rot Risk: " << analysis.rotRiskLevel << "\n";

	if (!analysis.recommendation.empty())
		std::cout << "\n  >> " << analysis.recommendation << "\n";

	std::cout << std::string(60, '=') << "\n";
}

bool AudioCDCopier::SaveDiscRotLog(const DiscRotAnalysis& analysis, const std::wstring& path) {
	FILE* f = nullptr;
	if (_wfopen_s(&f, path.c_str(), L"w") != 0 || !f)
		return false;

	fprintf(f, "# ==============================\n");
	fprintf(f, "# Disc Rot Analysis Report\n");
	fprintf(f, "# ==============================\n");
	fprintf(f, "#\n");
	fprintf(f, "# Risk Level:            %s\n", analysis.rotRiskLevel.c_str());
	if (!analysis.recommendation.empty())
		fprintf(f, "# Recommendation:        %s\n", analysis.recommendation.c_str());
	fprintf(f, "#\n");

	fprintf(f, "# --- Zone Error Rates ---\n");
	fprintf(f, "# Inner  (0-33%%%%):       %.2f%% (%d/%d)\n",
		analysis.zones.InnerErrorRate(), analysis.zones.innerErrors, analysis.zones.innerSectors);
	fprintf(f, "# Middle (33-66%%%%):      %.2f%% (%d/%d)\n",
		analysis.zones.MiddleErrorRate(), analysis.zones.middleErrors, analysis.zones.middleSectors);
	fprintf(f, "# Outer  (66-100%%%%):     %.2f%% (%d/%d)\n",
		analysis.zones.OuterErrorRate(), analysis.zones.outerErrors, analysis.zones.outerSectors);
	fprintf(f, "#\n");

	fprintf(f, "# --- Read Consistency ---\n");
	fprintf(f, "# Inconsistent Sectors:  %d / %d tested\n",
		analysis.inconsistentSectors, analysis.totalRereadTests);
	fprintf(f, "# Inconsistency Rate:    %.2f%%\n", analysis.inconsistencyRate);
	fprintf(f, "#\n");

	fprintf(f, "# --- Disc Rot Indicators ---\n");
	fprintf(f, "# Edge Concentration:    %s\n", analysis.edgeConcentration ? "YES" : "NO");
	fprintf(f, "# Progressive Pattern:   %s\n", analysis.progressivePattern ? "YES" : "NO");
	fprintf(f, "# Pinhole Pattern:       %s\n", analysis.pinholePattern ? "YES" : "NO");
	fprintf(f, "# Read Instability:      %s\n", analysis.readInstability ? "YES" : "NO");
	fprintf(f, "#\n");

	fprintf(f, "# ==============================\n");
	fprintf(f, "# Zone Summary\n");
	fprintf(f, "# ==============================\n");
	fprintf(f, "Zone,ErrorRate,Errors,TotalSectors\n");
	fprintf(f, "Inner (0-33%%),%.2f,%d,%d\n",
		analysis.zones.InnerErrorRate(), analysis.zones.innerErrors, analysis.zones.innerSectors);
	fprintf(f, "Middle (33-66%%),%.2f,%d,%d\n",
		analysis.zones.MiddleErrorRate(), analysis.zones.middleErrors, analysis.zones.middleSectors);
	fprintf(f, "Outer (66-100%%),%.2f,%d,%d\n",
		analysis.zones.OuterErrorRate(), analysis.zones.outerErrors, analysis.zones.outerSectors);
	fprintf(f, "\n");

	fprintf(f, "# ==============================\n");
	fprintf(f, "# Error Clusters (%zu total)\n", analysis.clusters.size());
	fprintf(f, "# ==============================\n");
	fprintf(f, "ClusterIndex,StartLBA,EndLBA,SectorCount,ErrorCount\n");
	for (size_t i = 0; i < analysis.clusters.size(); i++) {
		const auto& c = analysis.clusters[i];
		fprintf(f, "%zu,%lu,%lu,%d,%d\n",
			i, c.startLBA, c.endLBA, c.size(), c.errorCount);
	}

	fclose(f);
	return true;
}