#define NOMINMAX
#include "AudioCDCopier.h"
#include "InterruptHandler.h"
#include "ConsoleColor.h"
#include "ConsoleGraph.h"
#include "ConsoleFormat.h"
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

	// ── Phase 0 (optional): C1 quality scan for early degradation ────
	// C1 errors are correctable — they reveal disc stress BEFORE C2
	// errors appear.  Only available on Plextor/LiteOn/MediaTek drives.
	QCheckResult c1Result;
	bool hasC1 = false;

	if (m_drive.SupportsQCheck() || m_drive.SupportsLiteOnScan()) {
		std::cout << "Phase 0: C1 quality scan (early degradation detection)...\n";

		bool usePlextor = m_drive.SupportsQCheck();

		if (!usePlextor)
			m_drive.SetSpeed(scanSpeed);

		bool started = usePlextor
			? m_drive.PlextorQCheckStart(firstLBA, lastLBA)
			: m_drive.LiteOnScanStart(firstLBA, lastLBA);

		if (started) {
			bool scanDone = false;
			int sampleIndex = 0;
			DWORD lastReportedLBA = DWORD(-1);
			bool useLiteOn = !usePlextor;

			while (!scanDone) {
				if (g_interrupt.IsInterrupted() || g_interrupt.CheckEscapeKey())
					break;

				if (usePlextor)
					std::this_thread::sleep_for(std::chrono::milliseconds(500));

				int c1 = 0, c2 = 0, cu = 0;
				DWORD currentLBA = 0;

				bool pollOk = usePlextor
					? m_drive.PlextorQCheckPoll(c1, c2, cu, currentLBA, scanDone)
					: m_drive.LiteOnScanPoll(c1, c2, cu, currentLBA, scanDone);

				if (!pollOk) break;
				if (currentLBA == 0 && c1 == 0 && c2 == 0 && !scanDone) continue;
				if (currentLBA == lastReportedLBA && !scanDone) continue;
				lastReportedLBA = currentLBA;

				// Skip startup artifact
				if (useLiteOn && sampleIndex < 3) { sampleIndex++; continue; }

				if (useLiteOn && currentLBA >= lastLBA) scanDone = true;

				QCheckSample sample;
				sample.lba = currentLBA;
				sample.c1 = c1;
				sample.c2 = c2;
				sample.cu = cu;
				c1Result.samples.push_back(sample);
				c1Result.totalC1 += c1;
				c1Result.totalC2 += c2;
				if (c1 > c1Result.maxC1PerSecond) c1Result.maxC1PerSecond = c1;
				sampleIndex++;

				double pct = (totalSectors > 0)
					? std::min(100.0, static_cast<double>(currentLBA - firstLBA) * 100.0 / totalSectors)
					: 0.0;
				std::cout << "\r  C1 scan... " << std::fixed << std::setprecision(1)
					<< pct << "%  C1=" << c1 << "     " << std::flush;
			}

			if (usePlextor) m_drive.PlextorQCheckStop();
			else m_drive.LiteOnScanStop();

			c1Result.totalSeconds = static_cast<DWORD>(c1Result.samples.size());
			if (c1Result.totalSeconds > 0)
				c1Result.avgC1PerSecond = static_cast<double>(c1Result.totalC1) / c1Result.totalSeconds;

			hasC1 = !c1Result.samples.empty();
			if (hasC1)
				std::cout << "\r  C1 scan complete: " << c1Result.samples.size()
					<< " samples, avg C1=" << std::fixed << std::setprecision(1)
					<< c1Result.avgC1PerSecond << "/sec\n\n";
		}
	}
	else {
		std::cout << "  (C1 scan not available — drive lacks quality scan support)\n";
		std::cout << "  (Disc rot detection limited to C2 errors only)\n\n";
	}

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

	// ── Factor C1 data into rot assessment ───────────────────────────
	if (hasC1) {
		AnalyzeC1RotPatterns(c1Result, firstLBA, lastLBA, result);
	}

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

	// Print C1 graph if available
	if (hasC1 && !c1Result.samples.empty()) {
		// Convert samples to a flat int vector for BucketData
		std::vector<int> c1Values;
		c1Values.reserve(c1Result.samples.size());
		for (const auto& s : c1Result.samples)
			c1Values.push_back(s.c1);

		int maxC1 = 1;
		for (int v : c1Values)
			if (v > maxC1) maxC1 = v;

		// Ensure the chart is tall enough to show the Red Book reference line
		if (maxC1 < 250) maxC1 = 250;

		Console::GraphOptions c1Opts;
		c1Opts.title = "C1 Quality Profile";
		c1Opts.subtitle = "C1 = corrected errors \xe2\x80\x94 early warning for degradation";
		c1Opts.width = 60;
		c1Opts.height = 10;
		c1Opts.refLine = 220;
		c1Opts.refLabel = "Red Book limit (220/sec)";
		c1Opts.colorize = true;

		auto buckets = Console::BucketData(c1Values, c1Opts.width);
		Console::DrawBarGraph(buckets, maxC1, c1Opts,
			static_cast<DWORD>(c1Result.samples.size()));
	}

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

void AudioCDCopier::ClassifyZone(DWORD lba, DWORD totalStart, ULONG totalEnd,
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
	using namespace Console;

	std::cout << "\n";
	SetColorRGB(Theme::CyanR, Theme::CyanG, Theme::CyanB);
	std::cout << Sym::TopLeft;
	for (int i = 0; i < 58; i++) std::cout << Sym::Horizontal;
	std::cout << Sym::TopRight << "\n";
	Heading("  DISC ROT ANALYSIS REPORT\n");
	SetColorRGB(Theme::CyanR, Theme::CyanG, Theme::CyanB);
	std::cout << Sym::BottomLeft;
	for (int i = 0; i < 58; i++) std::cout << Sym::Horizontal;
	std::cout << Sym::BottomRight << "\n";
	Reset();

	std::cout << "\n";
	Heading("  Zone Error Rates\n");
	SetColorRGB(Theme::DimR, Theme::DimG, Theme::DimB);
	std::cout << "  (Disc surface divided into three radial zones)\n\n";
	Reset();

	auto printZone = [](const char* label, double rate, int errors, int sectors) {
		using namespace Console;

		// Label line
		SetColorRGB(Theme::WhiteR, Theme::WhiteG, Theme::WhiteB);
		std::cout << "  " << label;
		SetColorRGB(Theme::DimR, Theme::DimG, Theme::DimB);
		std::cout << "(" << errors << "/" << sectors << ")\n";

		// Bar
		constexpr int barWidth = 30;
		int fillLen = std::min(barWidth, static_cast<int>(rate * 3.0));
		if (rate > 0.0 && fillLen == 0) fillLen = 1; // guarantee 1 block for any errors
		double severity = std::min(1.0, rate / 10.0);

		std::cout << "    " << Sym::Vertical;
		for (int i = 0; i < barWidth; i++) {
			if (i < fillLen) {
				// Gradient: each filled cell gets progressively more severe color
				double cellSev = severity * (static_cast<double>(i + 1) / fillLen);
				SetBarColor(std::min(1.0, cellSev));
				std::cout << Sym::Bar8;
			}
			else {
				SetColorRGB(Theme::DimR, Theme::DimG, Theme::DimB);
				std::cout << Sym::BlockLight;
			}
		}
		SetColorRGB(Theme::DimR, Theme::DimG, Theme::DimB);
		std::cout << Sym::Vertical << " ";

		// Rate + status symbol
		if (rate > 5.0) {
			SetColorRGB(Theme::RedR, Theme::RedG, Theme::RedB);
			std::cout << std::fixed << std::setprecision(2) << rate << "% " << Sym::Cross << " severe";
		}
		else if (rate > 1.0) {
			SetColorRGB(Theme::YellowR, Theme::YellowG, Theme::YellowB);
			std::cout << std::fixed << std::setprecision(2) << rate << "% " << Sym::Warn << " moderate";
		}
		else {
			SetColorRGB(Theme::GreenR, Theme::GreenG, Theme::GreenB);
			std::cout << std::fixed << std::setprecision(2) << rate << "% " << Sym::Check << " healthy";
		}
		Reset();
		std::cout << "\n";
		};

	printZone("Inner  (0-33%):   ", analysis.zones.InnerErrorRate(),
		analysis.zones.innerErrors, analysis.zones.innerSectors);
	printZone("Middle (33-66%):  ", analysis.zones.MiddleErrorRate(),
		analysis.zones.middleErrors, analysis.zones.middleSectors);
	printZone("Outer  (66-100%): ", analysis.zones.OuterErrorRate(),
		analysis.zones.outerErrors, analysis.zones.outerSectors);

	std::cout << "\n";
	Heading("  Error Clusters\n");
	Reset();
	std::cout << "  Total clusters:  " << analysis.clusters.size() << "\n";
	if (!analysis.clusters.empty()) {
		int maxSize = 0;
		for (const auto& c : analysis.clusters)
			if (c.size() > maxSize) maxSize = c.size();
		std::cout << "  Largest cluster: " << maxSize << " sectors";
		if (maxSize > 100) { Error("  (severe - large contiguous damage)"); }
		else if (maxSize > 20) { Warning("  (moderate - localized damage)"); }
		else { Success("  (minor - small scratch or defect)"); }
		std::cout << "\n";
	}

	std::cout << "\n";
	Heading("  Disc Rot Indicators\n");
	Reset();

	auto indicator = [](bool v, const char* yesExplain, const char* noExplain) {
		using namespace Console;
		if (v) {
			SetColorRGB(Theme::RedR, Theme::RedG, Theme::RedB);
			std::cout << Sym::Cross << " YES  - " << yesExplain;
		}
		else {
			SetColorRGB(Theme::GreenR, Theme::GreenG, Theme::GreenB);
			std::cout << Sym::Check << " NO   - " << noExplain;
		}
		Reset();
		std::cout << "\n";
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
	if (analysis.readInstability) {
		SetColorRGB(Theme::RedR, Theme::RedG, Theme::RedB);
		std::cout << Sym::Cross << " YES  - Same sectors return different data on re-read ("
			<< static_cast<int>(analysis.inconsistencyRate) << "% unstable)\n";
	}
	else {
		SetColorRGB(Theme::GreenR, Theme::GreenG, Theme::GreenB);
		std::cout << Sym::Check << " NO   - Reads are consistent across re-reads\n";
	}
	Reset();

	std::cout << "\n";
	Heading("  Risk Assessment\n");
	Reset();
	std::cout << "  Disc Rot Risk: ";
	if (analysis.rotRiskLevel == "CRITICAL" || analysis.rotRiskLevel == "HIGH")
		SetColorRGB(Theme::RedR, Theme::RedG, Theme::RedB);
	else if (analysis.rotRiskLevel == "MODERATE")
		SetColorRGB(Theme::YellowR, Theme::YellowG, Theme::YellowB);
	else
		SetColorRGB(Theme::GreenR, Theme::GreenG, Theme::GreenB);
	std::cout << "\033[1m" << analysis.rotRiskLevel << "\033[22m\n";
	Reset();

	if (!analysis.recommendation.empty()) {
		SetColorRGB(Theme::CyanR, Theme::CyanG, Theme::CyanB);
		std::cout << "\n  " << Sym::Arrow << " " << analysis.recommendation << "\n";
		Reset();
	}

	SetColorRGB(Theme::CyanR, Theme::CyanG, Theme::CyanB);
	std::cout << Sym::BottomLeft;
	for (int i = 0; i < 58; i++) std::cout << Sym::Horizontal;
	std::cout << Sym::BottomRight << "\n";
	Reset();
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

void AudioCDCopier::AnalyzeC1RotPatterns(const QCheckResult& c1Result,
	DWORD firstLBA, DWORD lastLBA, DiscRotAnalysis& analysis) {
	if (c1Result.samples.empty()) return;

	DWORD range = lastLBA - firstLBA;
	if (range == 0) return;

	// Split C1 data into three zones and compute avg C1 per zone
	double innerC1 = 0, middleC1 = 0, outerC1 = 0;
	int innerN = 0, middleN = 0, outerN = 0;

	for (const auto& s : c1Result.samples) {
		double pos = (range > 0) ? static_cast<double>(s.lba - firstLBA) / range : 0;
		if (pos < 0.33) { innerC1 += s.c1; innerN++; }
		else if (pos < 0.66) { middleC1 += s.c1; middleN++; }
		else { outerC1 += s.c1; outerN++; }
	}

	double avgInner = innerN > 0 ? innerC1 / innerN : 0;
	double avgMiddle = middleN > 0 ? middleC1 / middleN : 0;
	double avgOuter = outerN > 0 ? outerC1 / outerN : 0;

	std::cout << "\n--- C1 Zone Analysis (early warning) ---\n";
	std::cout << "  Inner  avg C1/sec: " << std::fixed << std::setprecision(1) << avgInner << "\n";
	std::cout << "  Middle avg C1/sec: " << avgMiddle << "\n";
	std::cout << "  Outer  avg C1/sec: " << avgOuter << "\n";

	// C1-based rot indicators (these fire BEFORE C2 errors appear)
	bool c1EdgeElevated = (avgOuter > avgInner * 3.0) && (avgOuter > 10.0);
	bool c1Progressive = (avgInner < avgMiddle) && (avgMiddle < avgOuter) && (avgOuter > 10.0);
	bool c1OverallHigh = (c1Result.avgC1PerSecond > 50.0);
	bool c1RedBookFail = (c1Result.avgC1PerSecond >= 220.0);

	if (c1EdgeElevated)
		std::cout << "  ** C1 elevated at outer edge — early disc rot signal **\n";
	if (c1Progressive)
		std::cout << "  ** C1 rising inner→outer — progressive degradation pattern **\n";
	if (c1RedBookFail)
		std::cout << "  ** C1 exceeds Red Book limit — disc is stressed **\n";

	// Boost the rot risk score based on C1 findings
	// These are early warnings that wouldn't show up in C2 alone
	int c1Score = 0;
	if (c1EdgeElevated) c1Score += 15;
	if (c1Progressive) c1Score += 20;
	if (c1OverallHigh) c1Score += 10;
	if (c1RedBookFail) c1Score += 15;

	if (c1Score > 0) {
		// Re-assess with C1 data factored in
		std::string current = analysis.rotRiskLevel;
		if (current == "NONE" && c1Score >= 15)
			analysis.rotRiskLevel = "LOW";
		if (current == "NONE" && c1Score >= 30)
			analysis.rotRiskLevel = "MODERATE";
		if (current == "LOW" && c1Score >= 20)
			analysis.rotRiskLevel = "MODERATE";
		if (current == "MODERATE" && c1Score >= 25)
			analysis.rotRiskLevel = "HIGH";

		if (analysis.rotRiskLevel != current) {
			std::cout << "  Risk level upgraded from " << current
				<< " to " << analysis.rotRiskLevel
				<< " based on C1 early-warning data\n";
		}
	}
}