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
				ClassifyZone(lba, firstLBA, lastLBA, c2Errors, result.zones);
				if (c2Errors > 0) {
					errorLBAs.push_back(lba);
				}
			}
			else {
				errorLBAs.push_back(lba);
			}

			scannedSectors++;
			progress.Update(static_cast<int>(scannedSectors), static_cast<int>(totalSectors));
		}
	}
	progress.Finish(true);

	std::sort(errorLBAs.begin(), errorLBAs.end());
	DetectErrorClusters(errorLBAs, result.clusters);

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
			if (TestReadConsistency(lba, 3, inconsistent)) {
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

	result.rotRiskLevel = AssessRotRisk(result);

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

bool AudioCDCopier::TestReadConsistency(DWORD lba, int passes, int& mismatchCount) {
	mismatchCount = 0;
	if (passes < 2) return true;

	std::vector<BYTE> reference(AUDIO_SECTOR_SIZE);
	if (!m_drive.ReadSectorAudioOnly(lba, reference.data()))
		return false;

	std::vector<BYTE> compare(AUDIO_SECTOR_SIZE);
	for (int i = 1; i < passes; i++) {
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

void AudioCDCopier::DetectErrorClusters(const std::vector<DWORD>& errorLBAs,
	std::vector<ErrorCluster>& clusters) {
	clusters.clear();
	if (errorLBAs.empty()) return;

	ErrorCluster current;
	current.startLBA = errorLBAs[0];
	current.endLBA = errorLBAs[0];
	current.errorCount = 1;

	for (size_t i = 1; i < errorLBAs.size(); i++) {
		if (errorLBAs[i] <= current.endLBA + 5) {
			current.endLBA = errorLBAs[i];
			current.errorCount++;
		}
		else {
			clusters.push_back(current);
			current.startLBA = errorLBAs[i];
			current.endLBA = errorLBAs[i];
			current.errorCount = 1;
		}
	}
	clusters.push_back(current);
}

void AudioCDCopier::AnalyzeErrorPatterns(const std::vector<DWORD>& errorLBAs,
	DiscRotAnalysis& analysis) {
	if (errorLBAs.empty()) return;

	DetectErrorClusters(errorLBAs, analysis.clusters);

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
	std::cout << "  Inner  (0-33%):  " << analysis.zones.InnerErrorRate() << "% ("
		<< analysis.zones.innerErrors << "/" << analysis.zones.innerSectors << ")\n";
	std::cout << "  Middle (33-66%): " << analysis.zones.MiddleErrorRate() << "% ("
		<< analysis.zones.middleErrors << "/" << analysis.zones.middleSectors << ")\n";
	std::cout << "  Outer  (66-100%):" << analysis.zones.OuterErrorRate() << "% ("
		<< analysis.zones.outerErrors << "/" << analysis.zones.outerSectors << ")\n";

	std::cout << "\n--- Error Clusters ---\n";
	std::cout << "  Total clusters:  " << analysis.clusters.size() << "\n";
	if (!analysis.clusters.empty()) {
		int maxSize = 0;
		for (const auto& c : analysis.clusters)
			if (c.size() > maxSize) maxSize = c.size();
		std::cout << "  Largest cluster: " << maxSize << " sectors\n";
	}

	std::cout << "\n--- Disc Rot Indicators ---\n";
	auto yn = [](bool v) -> const char* { return v ? "YES" : "NO"; };
	std::cout << "  Edge concentration:    " << yn(analysis.edgeConcentration) << "\n";
	std::cout << "  Progressive pattern:   " << yn(analysis.progressivePattern) << "\n";
	std::cout << "  Pinhole pattern:       " << yn(analysis.pinholePattern) << "\n";
	std::cout << "  Read instability:      " << yn(analysis.readInstability) << "\n";
	std::cout << "  Inconsistency rate:    " << analysis.inconsistencyRate << "%\n";

	std::cout << "\n--- Risk Assessment ---\n";
	std::cout << "  Disc Rot Risk: " << analysis.rotRiskLevel << "\n";

	if (!analysis.recommendation.empty())
		std::cout << "  Recommendation: " << analysis.recommendation << "\n";

	std::cout << std::string(60, '=') << "\n";
}

bool AudioCDCopier::SaveDiscRotLog(const DiscRotAnalysis& analysis, const std::wstring& path) {
	FILE* f = nullptr;
	if (_wfopen_s(&f, path.c_str(), L"w") != 0 || !f)
		return false;

	fprintf(f, "Disc Rot Analysis Report\n");
	fprintf(f, "========================\n\n");

	fprintf(f, "Zone Error Rates:\n");
	fprintf(f, "  Inner:  %.2f%% (%d/%d)\n", analysis.zones.InnerErrorRate(),
		analysis.zones.innerErrors, analysis.zones.innerSectors);
	fprintf(f, "  Middle: %.2f%% (%d/%d)\n", analysis.zones.MiddleErrorRate(),
		analysis.zones.middleErrors, analysis.zones.middleSectors);
	fprintf(f, "  Outer:  %.2f%% (%d/%d)\n", analysis.zones.OuterErrorRate(),
		analysis.zones.outerErrors, analysis.zones.outerSectors);

	fprintf(f, "\nClusters: %zu\n", analysis.clusters.size());
	for (size_t i = 0; i < analysis.clusters.size(); i++) {
		fprintf(f, "  [%zu] LBA %lu-%lu (%d sectors, %d errors)\n", i,
			analysis.clusters[i].startLBA, analysis.clusters[i].endLBA,
			analysis.clusters[i].size(), analysis.clusters[i].errorCount);
	}

	fprintf(f, "\nIndicators:\n");
	fprintf(f, "  Edge concentration: %s\n", analysis.edgeConcentration ? "YES" : "NO");
	fprintf(f, "  Progressive: %s\n", analysis.progressivePattern ? "YES" : "NO");
	fprintf(f, "  Pinhole: %s\n", analysis.pinholePattern ? "YES" : "NO");
	fprintf(f, "  Instability: %s (%.2f%%)\n",
		analysis.readInstability ? "YES" : "NO", analysis.inconsistencyRate);
	fprintf(f, "\nRisk Level: %s\n", analysis.rotRiskLevel.c_str());

	fclose(f);
	return true;
}