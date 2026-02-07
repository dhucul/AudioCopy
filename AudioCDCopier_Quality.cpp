#define NOMINMAX
#include "AudioCDCopier.h"
#include "AccurateRip.h"
#include "InterruptHandler.h"
#include "MenuHelpers.h"
#include <iostream>
#include <fstream>
#include <iomanip>
#include <vector>
#include <algorithm>
#include <chrono>       // ✅ ADD THIS
#include <map>          // ✅ ADD THIS
#include <cstring>      // ✅ ADD THIS

// ============================================================================
// Quality Scanning
// ============================================================================

DWORD AudioCDCopier::CalculateTotalAudioSectors(const DiscInfo& disc) const {
	DWORD totalSectors = 0;
	for (const auto& t : disc.tracks) {
		if (t.isAudio) {
			DWORD start = (t.trackNumber == 1) ? 0 : t.pregapLBA;
			totalSectors += t.endLBA - start + 1;
		}
	}
	return totalSectors;
}

ScsiDrive::C2ReadOptions AudioCDCopier::BuildC2ReadOptions(int sensitivity, bool& useConditionalMultiPass) const {
	ScsiDrive::C2ReadOptions c2Opts;
	useConditionalMultiPass = false;

	switch (sensitivity) {
	case 1:  // Standard (improved)
		c2Opts.multiPass = false;
		c2Opts.countBytes = true;
		c2Opts.defeatCache = false;
		useConditionalMultiPass = false;
		break;
	case 2:  // PlexTools-style (with cache defeat)
		c2Opts.multiPass = false;
		c2Opts.countBytes = true;
		c2Opts.defeatCache = true;
		useConditionalMultiPass = true;
		break;
	case 3:  // PlexTools-style (multi-pass with cache defeat)
		c2Opts.multiPass = true;
		c2Opts.passCount = 2;
		c2Opts.countBytes = true;
		c2Opts.defeatCache = true;
		useConditionalMultiPass = false;
		break;
	case 4:  // Paranoid
		c2Opts.multiPass = false;
		c2Opts.countBytes = true;
		c2Opts.defeatCache = true;
		useConditionalMultiPass = true;
		break;
	default:
		c2Opts.countBytes = true;
		useConditionalMultiPass = false;
		break;
	}

	return c2Opts;
}

bool AudioCDCopier::RunC2ScanPass1(const DiscInfo& disc, const ScsiDrive::C2ReadOptions& c2Opts,
	DWORD totalSectors, std::vector<std::pair<DWORD, int>>& errorSectors,
	std::vector<DWORD>& pass1ErrorLBAs, int& totalC2Errors, DWORD& scannedSectors) {
	ProgressIndicator progress(40);
	progress.SetLabel("  C2 Scan (Pass 1)");
	progress.Start();

	for (const auto& t : disc.tracks) {
		if (!t.isAudio) continue;
		DWORD start = (t.trackNumber == 1) ? 0 : t.pregapLBA;

		for (DWORD lba = start; lba <= t.endLBA; lba++) {
			if (g_interrupt.IsInterrupted() || g_interrupt.CheckEscapeKey()) {
				g_interrupt.SetInterrupted(true);
				std::cout << "\n\n*** Scan cancelled by user ***\n";
				m_drive.SetSpeed(0);
				return false;
			}

			std::vector<BYTE> buf(AUDIO_SECTOR_SIZE);
			int c2Errors = 0;

			if (m_drive.ReadSectorWithC2Ex(lba, buf.data(), nullptr, c2Errors, nullptr, c2Opts)) {
				if (c2Errors > 0) {
					errorSectors.push_back({ lba, c2Errors });
					totalC2Errors += c2Errors;
					pass1ErrorLBAs.push_back(lba);
				}
			}
			else {
				errorSectors.push_back({ lba, -1 });
				pass1ErrorLBAs.push_back(lba);
			}

			scannedSectors++;
			progress.Update(static_cast<int>(scannedSectors), static_cast<int>(totalSectors));
		}
	}

	progress.Finish(true);
	return true;
}

void AudioCDCopier::RunConditionalC2ReRead(const ScsiDrive::C2ReadOptions& c2Opts, int sensitivity,
	std::vector<std::pair<DWORD, int>>& errorSectors, std::vector<DWORD>& pass1ErrorLBAs,
	int& totalC2Errors) {
	if (pass1ErrorLBAs.empty()) return;

	std::cout << "\nPass 2: Re-reading " << pass1ErrorLBAs.size() << " error sectors for verification...\n";

	ProgressIndicator progress(40);
	progress.SetLabel("  C2 Scan (Pass 2)");
	progress.Start();

	int pass2Count = 0;
	for (DWORD errorLBA : pass1ErrorLBAs) {
		if (g_interrupt.IsInterrupted() || g_interrupt.CheckEscapeKey()) {
			break;
		}

		DefeatDriveCache(errorLBA, 0);

		std::vector<BYTE> buf(AUDIO_SECTOR_SIZE);
		int c2Errors = 0;

		ScsiDrive::C2ReadOptions rereadOpts = c2Opts;
		rereadOpts.multiPass = (sensitivity == 4);
		rereadOpts.passCount = (sensitivity == 4) ? 3 : 1;
		rereadOpts.defeatCache = true;

		if (m_drive.ReadSectorWithC2Ex(errorLBA, buf.data(), nullptr, c2Errors, nullptr, rereadOpts)) {
			auto it = std::find_if(errorSectors.begin(), errorSectors.end(),
				[errorLBA](const std::pair<DWORD, int>& p) { return p.first == errorLBA; });

			if (it != errorSectors.end()) {
				int pass1Value = it->second;

				if (pass1Value < 0) {
					totalC2Errors += c2Errors;
					it->second = c2Errors;
				}
				else {
					totalC2Errors += (c2Errors - pass1Value);
					it->second = std::max(pass1Value, c2Errors);
				}
			}
		}

		pass2Count++;
		progress.Update(pass2Count, static_cast<int>(pass1ErrorLBAs.size()));
	}

	progress.Finish(true);
	std::cout << "Pass 2 complete - error sectors reverified.\n";
}

void AudioCDCopier::RunDualSpeedValidation(const DiscInfo& disc,
	const std::vector<std::pair<DWORD, int>>& errorSectors, int totalC2Errors, int scanSpeed) {
	std::vector<DWORD> highErrorLBAs;
	int errorThreshold = (totalC2Errors > 0 && !errorSectors.empty())
		? (totalC2Errors * 3 / static_cast<int>(errorSectors.size()))
		: 100;

	for (const auto& p : errorSectors) {
		if (p.second > errorThreshold) {
			highErrorLBAs.push_back(p.first);
		}
	}

	if (highErrorLBAs.empty() || highErrorLBAs.size() > 20) return;

	std::cout << "\nPhase 3: Dual-speed validation for " << highErrorLBAs.size()
		<< " high-error sectors...\n";

	ProgressIndicator progress(40);
	progress.SetLabel("  Dual-speed");
	progress.Start();

	ScsiDrive::C2ReadOptions dualSpeedOpts;
	dualSpeedOpts.countBytes = true;

	int speedTestCount = 0;
	for (DWORD testLBA : highErrorLBAs) {
		if (g_interrupt.IsInterrupted() || g_interrupt.CheckEscapeKey()) {
			break;
		}

		m_drive.SetSpeed(2);
		std::vector<BYTE> buf(AUDIO_SECTOR_SIZE);
		int slowC2 = 0;
		bool slowSuccess = m_drive.ReadSectorWithC2Ex(testLBA, buf.data(), nullptr, slowC2, nullptr, dualSpeedOpts);

		DefeatDriveCache(testLBA, 0);

		m_drive.SetSpeed(16);
		int fastC2 = 0;
		bool fastSuccess = m_drive.ReadSectorWithC2Ex(testLBA, buf.data(), nullptr, fastC2, nullptr, dualSpeedOpts);

		if (slowSuccess && fastSuccess && slowC2 > 0 && fastC2 == 0) {
			std::cout << "  LBA " << testLBA << ": Potential surface degradation "
				<< "(slow: " << slowC2 << ", fast: " << fastC2 << " errors)\n";
		}

		speedTestCount++;
		progress.Update(speedTestCount, static_cast<int>(highErrorLBAs.size()));
	}

	progress.Finish(true);
	m_drive.SetSpeed(scanSpeed);
}

void AudioCDCopier::PrintC2ScanReport(const DiscInfo& disc, int sensitivity, int scanSpeed,
	const ScsiDrive::C2ReadOptions& c2Opts, bool useConditionalMultiPass,
	const std::vector<std::pair<DWORD, int>>& errorSectors,
	const std::vector<DWORD>& pass1ErrorLBAs, DWORD scannedSectors) {
	std::cout << "\n" << std::string(60, '=') << "\n";
	std::cout << "              C2 ERROR SCAN REPORT\n";
	std::cout << std::string(60, '=') << "\n";

	const char* modeNames[] = { "Default", "Standard", "PlexTools (cache defeat)",
								"PlexTools (multi-pass)", "Paranoid" };
	int modeIndex = (sensitivity >= 0 && sensitivity <= 4) ? sensitivity : 0;

	std::cout << "\n--- Scan Configuration ---\n";
	std::cout << "  Mode:       " << modeNames[modeIndex] << "\n";
	std::cout << "  Speed:      " << (scanSpeed == 0 ? "Max" : std::to_string(scanSpeed) + "x") << "\n";
	if (sensitivity == 3)
		std::cout << "  Multi-pass: " << c2Opts.passCount << " passes with cache defeat\n";
	else if (useConditionalMultiPass)
		std::cout << "  Verify:     Re-read error sectors with cache defeat\n";

	int finalErrorSectors = 0;
	int finalReadFailures = 0;
	int finalC2Total = 0;
	int maxC2InSector = 0;
	DWORD worstLBA = 0;

	for (const auto& p : errorSectors) {
		if (p.second > 0) {
			finalErrorSectors++;
			finalC2Total += p.second;
			if (p.second > maxC2InSector) {
				maxC2InSector = p.second;
				worstLBA = p.first;
			}
		}
		else if (p.second == -1) {
			finalReadFailures++;
		}
	}

	std::cout << "\n--- Overall Results ---\n";
	std::cout << "  Sectors scanned:      " << scannedSectors << "\n";
	std::cout << "  Sectors with C2:      " << finalErrorSectors;
	if (scannedSectors > 0)
		std::cout << " (" << std::fixed << std::setprecision(3)
		<< (finalErrorSectors * 100.0 / scannedSectors) << "%)";
	std::cout << "\n";
	std::cout << "  Read failures:        " << finalReadFailures << "\n";
	std::cout << "  Total C2 errors:      " << finalC2Total << "\n";
	if (maxC2InSector > 0)
		std::cout << "  Worst sector:         LBA " << worstLBA << " (" << maxC2InSector << " errors)\n";
	if (scannedSectors > 0)
		std::cout << "  Error rate:           " << std::fixed << std::setprecision(2)
		<< (finalC2Total * 1000.0 / scannedSectors) << " errors/1000 sectors\n";
	if (useConditionalMultiPass)
		std::cout << "  Multi-pass verified:  " << pass1ErrorLBAs.size() << " sectors re-read\n";

	std::cout << "\n--- Per-Track Breakdown ---\n";
	std::cout << "  Track  Sectors   C2 Errors  Error Sectors  Quality\n";
	std::cout << "  " << std::string(55, '-') << "\n";
	for (const auto& t : disc.tracks) {
		if (!t.isAudio) continue;
		DWORD tStart = (t.trackNumber == 1) ? 0 : t.pregapLBA;
		DWORD tEnd = t.endLBA;
		DWORD tSectors = tEnd - tStart + 1;

		int trackC2 = 0;
		int trackErrorSectors = 0;
		int trackFailures = 0;
		for (const auto& p : errorSectors) {
			if (p.first >= tStart && p.first <= tEnd) {
				if (p.second > 0) {
					trackC2 += p.second;
					trackErrorSectors++;
				}
				else if (p.second == -1) {
					trackFailures++;
				}
			}
		}

		const char* quality = "Perfect";
		if (trackFailures > 0) quality = "BAD";
		else if (trackC2 > 50) quality = "Poor";
		else if (trackC2 > 10) quality = "Fair";
		else if (trackC2 > 0) quality = "Good";

		std::cout << "  " << std::setw(3) << t.trackNumber << "    "
			<< std::setw(7) << tSectors << "   "
			<< std::setw(7) << trackC2 << "     "
			<< std::setw(7) << trackErrorSectors;
		if (trackFailures > 0)
			std::cout << " (+" << trackFailures << " fail)";
		std::cout << "     " << quality << "\n";
	}

	if (!errorSectors.empty()) {
		auto sorted = errorSectors;
		std::sort(sorted.begin(), sorted.end(),
			[](const std::pair<DWORD, int>& a, const std::pair<DWORD, int>& b) {
				return a.second > b.second;
			});

		int showCount = std::min(10, static_cast<int>(sorted.size()));
		std::cout << "\n--- Top " << showCount << " Worst Sectors ---\n";
		for (int i = 0; i < showCount; i++) {
			DWORD lba = sorted[i].first;
			int errs = sorted[i].second;

			int trackNum = 0;
			for (const auto& t : disc.tracks) {
				DWORD tStart = (t.trackNumber == 1) ? 0 : t.pregapLBA;
				if (lba >= tStart && lba <= t.endLBA) { trackNum = t.trackNumber; break; }
			}

			if (errs == -1)
				std::cout << "  LBA " << std::setw(8) << lba << "  (Track " << std::setw(2) << trackNum << ")  READ FAILURE\n";
			else
				std::cout << "  LBA " << std::setw(8) << lba << "  (Track " << std::setw(2) << trackNum << ")  " << errs << " C2 errors\n";
		}
	}

	if (finalErrorSectors > 0 || finalReadFailures > 0) {
		DWORD firstLBA = 0, lastLBA = 0;
		for (const auto& t : disc.tracks) {
			if (t.isAudio) {
				DWORD s = (t.trackNumber == 1) ? 0 : t.pregapLBA;
				if (firstLBA == 0) firstLBA = s;
				lastLBA = t.endLBA;
			}
		}
		DWORD range = lastLBA - firstLBA;
		int innerErrs = 0, middleErrs = 0, outerErrs = 0;
		for (const auto& p : errorSectors) {
			if (p.second == 0) continue;
			double pct = range > 0 ? static_cast<double>(p.first - firstLBA) / range : 0;
			if (pct < 0.33) innerErrs++;
			else if (pct < 0.66) middleErrs++;
			else outerErrs++;
		}
		int totalErrs = innerErrs + middleErrs + outerErrs;

		std::cout << "\n--- Error Distribution ---\n";
		std::cout << "  Inner  (0-33%):   " << std::setw(4) << innerErrs << " error sectors";
		if (totalErrs > 0 && innerErrs * 100 / totalErrs > 50) std::cout << "  << concentration";
		std::cout << "\n";
		std::cout << "  Middle (33-66%):  " << std::setw(4) << middleErrs << " error sectors\n";
		std::cout << "  Outer  (66-100%): " << std::setw(4) << outerErrs << " error sectors";
		if (totalErrs > 0 && outerErrs * 100 / totalErrs > 50) std::cout << "  << concentration";
		std::cout << "\n";
	}

	std::cout << "\n" << std::string(60, '-') << "\n";
	std::cout << "  QUALITY: ";
	if (finalReadFailures > 0) {
		std::cout << "BAD - Unreadable sectors detected\n";
		std::cout << "  Recommendation: Rip with secure mode (Paranoid). Some data may be lost.\n";
	}
	else if (finalErrorSectors == 0) {
		std::cout << "PERFECT - No C2 errors detected\n";
		std::cout << "  Disc reads cleanly. Standard rip mode is sufficient.\n";
	}
	else if (finalC2Total < 20 && finalErrorSectors < 5) {
		std::cout << "GOOD - Minor C2 errors, fully correctable\n";
		std::cout << "  Standard rip mode should produce a perfect copy.\n";
	}
	else if (finalC2Total < 200 && finalReadFailures == 0) {
		std::cout << "FAIR - Moderate C2 errors detected\n";
		std::cout << "  Recommend secure rip mode for best results.\n";
	}
	else {
		std::cout << "POOR - Significant C2 errors\n";
		std::cout << "  Use Paranoid rip mode. Consider cleaning the disc.\n";
	}
	std::cout << std::string(60, '=') << "\n";
}

bool AudioCDCopier::ScanDiscForC2Errors(const DiscInfo& disc, int scanSpeed, int sensitivity) {
	std::cout << "\n=== C2 Error Scan ===\n";
	if (!m_drive.CheckC2Support()) {
		std::cout << "ERROR: Your drive does not support C2 error reporting.\n";
		return false;
	}

	DWORD totalSectors = CalculateTotalAudioSectors(disc);
	if (totalSectors == 0) {
		std::cout << "No audio tracks to scan.\n";
		return false;
	}

	if (!m_drive.ValidateC2Accuracy(disc.tracks[0].startLBA)) {
		std::cout << "WARNING: C2 reporting may be unreliable on this drive.\n";
	}

	std::cout << "Total audio sectors to scan: " << totalSectors << "\n";
	std::cout << "  (Press ESC or Ctrl+C to cancel)\n\n";
	m_drive.SetSpeed(scanSpeed);

	bool useConditionalMultiPass = false;
	ScsiDrive::C2ReadOptions c2Opts = BuildC2ReadOptions(sensitivity, useConditionalMultiPass);

	std::vector<std::pair<DWORD, int>> errorSectors;
	std::vector<DWORD> pass1ErrorLBAs;
	int totalC2Errors = 0;
	DWORD scannedSectors = 0;

	if (!RunC2ScanPass1(disc, c2Opts, totalSectors, errorSectors, pass1ErrorLBAs, totalC2Errors, scannedSectors)) {
		return false;
	}

	if (useConditionalMultiPass) {
		RunConditionalC2ReRead(c2Opts, sensitivity, errorSectors, pass1ErrorLBAs, totalC2Errors);
	}

	RunDualSpeedValidation(disc, errorSectors, totalC2Errors, scanSpeed);

	m_drive.SetSpeed(0);
	PrintC2ScanReport(disc, sensitivity, scanSpeed, c2Opts, useConditionalMultiPass, errorSectors, pass1ErrorLBAs, scannedSectors);

	return true;
}

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

	// IMPROVEMENT 1: Adaptive Zone-Based Sampling
	std::cout << "\nPhase 2: Adaptive read consistency check...\n";

	double innerRate = result.zones.InnerErrorRate();
	double middleRate = result.zones.MiddleErrorRate();
	double outerRate = result.zones.OuterErrorRate();

	auto calcSampleInterval = [](double errorRate) -> int {
		if (errorRate > 2.0) return 20;      // High errors: sample every 20 sectors (~5%)
		if (errorRate > 0.5) return 50;      // Medium errors: sample every 50 sectors (~2%)
		if (errorRate > 0.1) return 100;     // Low errors: sample every 100 sectors (~1%)
		return 200;                           // Clean zones: sample every 200 sectors (~0.5%)
		};

	int innerInterval = calcSampleInterval(innerRate);
	int middleInterval = calcSampleInterval(middleRate);
	int outerInterval = calcSampleInterval(outerRate);

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
			int sampleInterval = 200;  // default

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

			progress.Update(samplesChecked, 150);  // Estimated ~150 samples
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
// Additional Tests
// ============================================================================

bool AudioCDCopier::RunSpeedComparisonTest(DiscInfo& disc, std::vector<SpeedComparisonResult>& results) {
	std::cout << "\n=== Speed Comparison Test ===\n";
	if (!m_drive.CheckC2Support()) { std::cout << "ERROR: C2 support required.\n"; return false; }

	DWORD totalSectors = 0;
	for (const auto& t : disc.tracks) {
		if (t.isAudio) totalSectors += t.endLBA - ((t.trackNumber == 1) ? 0 : t.pregapLBA) + 1;
	}
	if (totalSectors == 0) return false;

	int sampleInterval = std::max(1, static_cast<int>(totalSectors / 100));
	results.clear();
	int tested = 0;

	for (const auto& t : disc.tracks) {
		if (!t.isAudio) continue;
		DWORD start = (t.trackNumber == 1) ? 0 : t.pregapLBA;
		for (DWORD lba = start; lba <= t.endLBA; lba += sampleInterval) {
			SpeedComparisonResult r = { lba, 0, 0, false };
			std::vector<BYTE> buf(SECTOR_WITH_C2_SIZE);
			m_drive.SetSpeed(4); Sleep(20);
			m_drive.ReadSectorWithC2(lba, buf.data(), nullptr, r.lowSpeedC2);
			m_drive.SetSpeed(24); Sleep(20);
			m_drive.ReadSectorWithC2(lba, buf.data(), nullptr, r.highSpeedC2);
			r.inconsistent = (r.highSpeedC2 > r.lowSpeedC2 * 2 && r.highSpeedC2 > 10);
			if (r.lowSpeedC2 > 0 || r.highSpeedC2 > 0) results.push_back(r);
			tested++;
		}
	}
	m_drive.SetSpeed(0);
	std::cout << "Tested " << tested << " sectors\n";

	// Analyze results and determine optimal speed
	int lowSpeedErrors = 0, highSpeedErrors = 0, inconsistentCount = 0;
	for (const auto& r : results) {
		lowSpeedErrors += r.lowSpeedC2;
		highSpeedErrors += r.highSpeedC2;
		if (r.inconsistent) inconsistentCount++;
	}

	std::cout << "\n=== Speed Test Results ===\n";
	std::cout << "Low speed (4x) total C2 errors:  " << lowSpeedErrors << "\n";
	std::cout << "High speed (24x) total C2 errors: " << highSpeedErrors << "\n";
	std::cout << "Inconsistent sectors: " << inconsistentCount << "\n\n";

	std::cout << "Recommended optimal speed: ";
	if (results.empty() || (lowSpeedErrors == 0 && highSpeedErrors == 0)) {
		std::cout << "24x (disc reads cleanly at any speed)\n";
	}
	else if (highSpeedErrors > lowSpeedErrors * 2 || inconsistentCount > tested / 10) {
		std::cout << "4x (significant errors at high speed)\n";
	}
	else if (highSpeedErrors > lowSpeedErrors) {
		std::cout << "8-12x (moderate speed recommended)\n";
	}
	else {
		std::cout << "24x (no significant benefit from slower speeds)\n";
	}

	return true;
}

bool AudioCDCopier::CheckLeadAreas(DiscInfo& disc, int scanSpeed) {
	std::cout << "\n=== Lead-in/Lead-out Area Check ===\n";
	if (!m_drive.CheckC2Support()) return false;
	m_drive.SetSpeed(scanSpeed);  // <-- USE PARAMETER

	int leadInErrors = 0, leadOutErrors = 0;
	for (DWORD lba = 0; lba < 150; lba++) {
		std::vector<BYTE> buf(SECTOR_WITH_C2_SIZE);
		int c2 = 0;
		if (m_drive.ReadSectorWithC2(lba, buf.data(), nullptr, c2) && c2 > 0) leadInErrors++;
	}

	DWORD leadOutStart = disc.leadOutLBA > 150 ? disc.leadOutLBA - 150 : 0;
	for (DWORD lba = leadOutStart; lba < disc.leadOutLBA; lba++) {
		std::vector<BYTE> buf(SECTOR_WITH_C2_SIZE);
		int c2 = 0;
		if (m_drive.ReadSectorWithC2(lba, buf.data(), nullptr, c2) && c2 > 0) leadOutErrors++;
	}
	m_drive.SetSpeed(0);
	std::cout << "Lead-in errors: " << leadInErrors << ", Lead-out errors: " << leadOutErrors << "\n";
	return (leadInErrors == 0 && leadOutErrors == 0);
}

void AudioCDCopier::GenerateSurfaceMap(DiscInfo& disc, const std::wstring& filename, int scanSpeed) {
	std::cout << "\n=== Generating Disc Surface Map ===\n";
	// ... validation code ...
	m_drive.SetSpeed(scanSpeed);  // <-- USE PARAMETER

	DWORD totalSectors = 0;
	for (const auto& t : disc.tracks) {
		if (t.isAudio) totalSectors += t.endLBA - ((t.trackNumber == 1) ? 0 : t.pregapLBA) + 1;
	}

	if (totalSectors == 0) {
		std::cout << "No audio tracks to scan.\n";
		return;
	}

	std::ofstream mapFile;
	if (!filename.empty()) {
		mapFile.open(filename);
		if (!mapFile) {
			std::cout << "ERROR: Cannot create map file.\n";
			return;
		}
		mapFile << "LBA,C2_Errors,Status\n";
	}

	m_drive.SetSpeed(8);
	ProgressIndicator progress(40);
	progress.SetLabel("  Surface Scan");
	progress.Start();

	DWORD scannedSectors = 0;
	for (const auto& t : disc.tracks) {
		if (!t.isAudio) continue;
		DWORD start = (t.trackNumber == 1) ? 0 : t.pregapLBA;

		for (DWORD lba = start; lba <= t.endLBA; lba++) {
			if (g_interrupt.IsInterrupted() || g_interrupt.CheckEscapeKey()) {
				m_drive.SetSpeed(0);
				progress.Finish(false);
				return;
			}

			std::vector<BYTE> buf(SECTOR_WITH_C2_SIZE);
			int c2Errors = 0;
			bool readOk = m_drive.ReadSectorWithC2(lba, buf.data(), nullptr, c2Errors);

			if (mapFile.is_open()) {
				mapFile << lba << "," << c2Errors << ","
					<< (readOk ? "OK" : "FAIL") << "\n";
			}

			scannedSectors++;
			progress.Update(static_cast<int>(scannedSectors), static_cast<int>(totalSectors));
		}
	}

	progress.Finish(true);
	m_drive.SetSpeed(0);

	std::cout << "Surface map generation complete.\n";
	if (!filename.empty()) {
		std::wcout << L"Map saved: " << filename << L"\n";
	}
}

// REPLACE line 1917:
bool AudioCDCopier::RunMultiPassVerification(DiscInfo& disc, std::vector<MultiPassResult>& results, int passes, int scanSpeed) {
	std::cout << "\n=== Multi-Pass Verification (" << passes << " passes) ===\n";
	std::cout << "Testing read consistency with hash-based comparison...\n\n";
	results.clear();

	// Use user-selected speed for accurate reading
	m_drive.SetSpeed(scanSpeed);  // <-- USE PARAMETER


	DWORD totalSectors = 0;
	for (const auto& t : disc.tracks) {
		if (t.isAudio) totalSectors += t.endLBA - ((t.trackNumber == 1) ? 0 : t.pregapLBA) + 1;
	}

	// Test more samples for better accuracy (every 25 sectors instead of 500)
	int sampleInterval = std::max(1, static_cast<int>(totalSectors / 1000));
	int tested = 0, perfectMatches = 0, partialMatches = 0, failures = 0;

	ProgressIndicator progress(40);
	progress.SetLabel("  Multi-Pass");
	progress.Start();

	for (const auto& t : disc.tracks) {
		if (!t.isAudio) continue;
		DWORD start = (t.trackNumber == 1) ? 0 : t.pregapLBA;

		for (DWORD lba = start; lba <= t.endLBA; lba += sampleInterval) {
			std::vector<std::vector<BYTE>> reads(passes);
			std::map<uint32_t, int> hashCounts;  // Track hash occurrences
			bool readSuccess = true;

			// Perform multiple reads with cache defeat
			for (int i = 0; i < passes; i++) {
				reads[i].resize(AUDIO_SECTOR_SIZE);

				// Cache defeat: read a random distant sector between passes
				if (i > 0 && lba + 10000 < disc.tracks.back().endLBA) {
					std::vector<BYTE> dummy(AUDIO_SECTOR_SIZE);
					m_drive.ReadSectorAudioOnly(lba + 10000 + (i * 1000), dummy.data());
				}

				if (!m_drive.ReadSectorAudioOnly(lba, reads[i].data())) {
					readSuccess = false;
					break;
				}

				// Calculate hash for this read
				uint32_t hash = CalculateSectorHash(reads[i].data());
				hashCounts[hash]++;

				Sleep(10);  // Small delay between reads
			}

			if (!readSuccess) {
				failures++;
				continue;
			}

			// Find majority hash (most common result)
			uint32_t majorityHash = 0;
			int maxCount = 0;
			for (const auto& [hash, count] : hashCounts) {
				if (count > maxCount) {
					maxCount = count;
					majorityHash = hash;
				}
			}

			// Count how many passes match the majority
			int matchCount = hashCounts[majorityHash];
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
			else if (matchCount >= passes / 2) {
				partialMatches++;
				results.push_back(r);  // Only log inconsistencies
			}
			else {
				failures++;
				results.push_back(r);
			}

			tested++;
			progress.Update(tested, totalSectors / sampleInterval);
		}
	}

	progress.Finish(true);
	m_drive.SetSpeed(0);

	// Print detailed results
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

	// Show most problematic sectors
	if (!results.empty()) {
		std::cout << "\n=== Most Inconsistent Sectors ===\n";
		std::sort(results.begin(), results.end(),
			[](const MultiPassResult& a, const MultiPassResult& b) {
				return a.passesMatched < b.passesMatched;
			});

		int shown = 0;
		for (const auto& r : results) {
			if (shown++ >= 10) break;
			std::cout << "  LBA " << std::setw(6) << r.lba
				<< ": " << r.passesMatched << "/" << r.totalPasses
				<< " matches (hash: " << std::hex << r.majorityHash << std::dec << ")\n";
		}
	}

	return true;
}


uint32_t AudioCDCopier::CalculateSectorHash(const BYTE* data) {
	// Simple FNV-1a hash for fast sector comparison
	uint32_t hash = 2166136261u;
	for (int i = 0; i < AUDIO_SECTOR_SIZE; i++) {
		hash ^= data[i];
		hash *= 16777619u;
	}
	return hash;
}


bool AudioCDCopier::FlushDriveCache() {
	BYTE cdb[10] = { 0x35, 0, 0, 0, 0, 0, 0, 0, 0, 0 };  // SYNCHRONIZE CACHE(10)
	return m_drive.SendSCSI(cdb, 10, nullptr, 0);
}

bool AudioCDCopier::AnalyzeAudioContent(DiscInfo& disc, AudioAnalysisResult& result, int scanSpeed) {
	std::cout << "\n=== Audio Content Analysis ===\n";
	result = AudioAnalysisResult{};
	m_drive.SetSpeed(scanSpeed);  // <-- USE PARAMETER

	for (const auto& t : disc.tracks) {
		if (!t.isAudio) continue;
		DWORD start = (t.trackNumber == 1) ? 0 : t.pregapLBA;
		for (DWORD lba = start; lba <= t.endLBA; lba += 100) {
			std::vector<BYTE> buf(AUDIO_SECTOR_SIZE);
			if (m_drive.ReadSectorAudioOnly(lba, buf.data())) {
				if (IsSectorSilent(buf.data())) result.silentSectors++;
				if (IsSectorClipped(buf.data())) result.clippedSectors++;
			}
		}
	}
	m_drive.SetSpeed(0);
	std::cout << "Silent: " << result.silentSectors << ", Clipped: " << result.clippedSectors << "\n";
	return true;
}

bool AudioCDCopier::IsSectorSilent(const BYTE* data) {
	for (int i = 0; i < AUDIO_SECTOR_SIZE; i += 4) {
		int16_t left = *reinterpret_cast<const int16_t*>(data + i);
		int16_t right = *reinterpret_cast<const int16_t*>(data + i + 2);
		if (std::abs(left) > 10 || std::abs(right) > 10) return false;
	}
	return true;
}

bool AudioCDCopier::IsSectorClipped(const BYTE* data) {
	int clippedSamples = 0;
	for (int i = 0; i < AUDIO_SECTOR_SIZE; i += 2) {
		int16_t sample = *reinterpret_cast<const int16_t*>(data + i);
		if (sample == 32767 || sample == -32768) clippedSamples++;
	}
	return clippedSamples > 10;
}

bool AudioCDCopier::RunComprehensiveScan(DiscInfo& disc, ComprehensiveScanResult& result, int speed) {
	std::cout << "\n=== COMPREHENSIVE DISC QUALITY SCAN ===\n";
	result = ComprehensiveScanResult{};

	RunBlerScan(disc, result.bler, speed);
	RunDiscRotScan(disc, result.rot, speed);
	RunSpeedComparisonTest(disc, result.speedComparison);
	RunMultiPassVerification(disc, result.multiPass, 3);
	AnalyzeAudioContent(disc, result.audio);

	result.overallScore = CalculateOverallScore(result);
	result.overallRating = (result.overallScore >= 90) ? "A" : (result.overallScore >= 70) ? "B" : "C";
	result.summary = "Scan complete. Score: " + std::to_string(result.overallScore);

	PrintComprehensiveReport(result);
	return true;
}

void AudioCDCopier::PrintComprehensiveReport(const ComprehensiveScanResult& result) {
	std::cout << "\n=== COMPREHENSIVE REPORT ===\n";
	std::cout << "Overall Score: " << result.overallScore << "/100\n";
	std::cout << "Grade: " << result.overallRating << "\n";
	std::cout << result.summary << "\n";
}

bool AudioCDCopier::SaveComprehensiveReport(const ComprehensiveScanResult& result, const std::wstring& filename) {
	std::ofstream file(filename);
	if (!file) return false;
	file << "COMPREHENSIVE DISC REPORT\n";
	file << "Score: " << result.overallScore << "/100\n";
	file << "Grade: " << result.overallRating << "\n";
	file.close();
	return true;
}

int AudioCDCopier::CalculateOverallScore(const ComprehensiveScanResult& result) {
	int score = 100;

	if (result.bler.totalC2Errors > 0) score -= std::min(30, result.bler.totalC2Errors / 100);
	if (result.bler.totalReadFailures > 0) score -= std::min(40, result.bler.totalReadFailures * 10);

	if (result.rot.edgeConcentration) score -= 10;
	if (result.rot.progressivePattern) score -= 15;
	if (result.rot.readInstability) score -= 20;

	score -= static_cast<int>(result.rot.inconsistencyRate * 2);

	return std::max(0, std::min(100, score));
}

bool AudioCDCopier::RunSeekTimeAnalysis(DiscInfo& disc, std::vector<SeekTimeResult>& results) {
	std::cout << "\n=== Seek Time Analysis ===\n";
	results.clear();

	DWORD totalSectors = 0;
	for (const auto& t : disc.tracks) {
		if (t.isAudio) totalSectors += t.endLBA - ((t.trackNumber == 1) ? 0 : t.pregapLBA) + 1;
	}

	if (totalSectors == 0) {
		std::cout << "No audio tracks to analyze.\n";
		return false;
	}

	m_drive.SetSpeed(0);
	std::cout << "Testing seek times across disc surface...\n";

	std::vector<DWORD> testPositions;
	for (int i = 0; i <= 10; i++) {
		testPositions.push_back(static_cast<DWORD>((totalSectors * i) / 10));
	}

	ProgressIndicator progress(40);
	progress.SetLabel("  Seek Test");
	progress.Start();

	int tested = 0;
	for (size_t i = 0; i < testPositions.size(); i++) {
		for (size_t j = 0; j < testPositions.size(); j++) {
			if (i == j) continue;

			if (g_interrupt.IsInterrupted() || g_interrupt.CheckEscapeKey()) {
				m_drive.SetSpeed(0);
				progress.Finish(false);
				return false;
			}

			DWORD fromLBA = testPositions[i];
			DWORD toLBA = testPositions[j];

			std::vector<BYTE> buf(AUDIO_SECTOR_SIZE);
			m_drive.ReadSectorAudioOnly(fromLBA, buf.data());

			auto startTime = std::chrono::high_resolution_clock::now();
			bool readOk = m_drive.ReadSectorAudioOnly(toLBA, buf.data());
			auto endTime = std::chrono::high_resolution_clock::now();

			double seekMs = std::chrono::duration<double, std::milli>(endTime - startTime).count();

			SeekTimeResult r;
			r.fromLBA = fromLBA;
			r.toLBA = toLBA;
			r.seekTimeMs = seekMs;
			r.abnormal = !readOk || (seekMs > 500.0);  // Mark as abnormal if failed or very slow
			results.push_back(r);

			tested++;
			progress.Update(tested, static_cast<int>(testPositions.size() * (testPositions.size() - 1)));
		}
	}

	progress.Finish(true);
	m_drive.SetSpeed(0);

	double avgSeek = 0, maxSeek = 0;
	int abnormalCount = 0;
	for (const auto& r : results) {
		avgSeek += r.seekTimeMs;
		if (r.seekTimeMs > maxSeek) maxSeek = r.seekTimeMs;
		if (r.abnormal) abnormalCount++;
	}
	avgSeek /= results.size();

	std::cout << "Tests performed: " << results.size() << "\n";
	std::cout << "Average seek time: " << std::fixed << std::setprecision(1) << avgSeek << " ms\n";
	std::cout << "Maximum seek time: " << maxSeek << " ms\n";
	if (abnormalCount > 0) std::cout << "Abnormal seeks: " << abnormalCount << "\n";

	return true;
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

	// Use requested scan speed
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

	// Real-time throughput tracking
	auto startTime = std::chrono::steady_clock::now();
	auto lastSpeedUpdate = startTime;
	constexpr double CD_1X_BYTES_PER_SEC = 176400.0;  // 1x CD speed in bytes/sec

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
				if (qTrack != t.trackNumber && qTrack != 0) {
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

			// Update progress with real-time speed every 500ms
			auto now = std::chrono::steady_clock::now();
			auto timeSinceUpdate = std::chrono::duration<double>(now - lastSpeedUpdate).count();

			if (timeSinceUpdate >= 0.5) {
				double totalElapsed = std::chrono::duration<double>(now - startTime).count();
				if (totalElapsed > 0) {
					double bytesRead = scannedSectors * 2352.0;
					double actualSpeedX = bytesRead / (totalElapsed * CD_1X_BYTES_PER_SEC);

					// Update progress label with speed info
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

	// Calculate final throughput
	auto endTime = std::chrono::steady_clock::now();
	double totalSeconds = std::chrono::duration<double>(endTime - startTime).count();
	double avgSpeedX = 0.0;
	if (totalSeconds > 0) {
		double totalBytes = scannedSectors * 2352.0;
		avgSpeedX = totalBytes / (totalSeconds * CD_1X_BYTES_PER_SEC);
	}

	// Report drive-reported vs measured speed
	std::cout << "\n=== Speed Verification ===\n";
	std::cout << "Requested speed: " << (scanSpeed == 0 ? "Max" : std::to_string(scanSpeed) + "x") << "\n";
	std::cout << "Measured throughput: " << std::fixed << std::setprecision(1) << avgSpeedX << "x\n";

	WORD actualRead = 0, actualWrite = 0;
	if (m_drive.GetActualSpeed(actualRead, actualWrite)) {
		double reportedSpeedX = actualRead / 176.4;
		std::cout << "Drive-reported speed: " << std::fixed << std::setprecision(1)
			<< reportedSpeedX << "x (" << actualRead << " KB/s)\n";

		// Warn if there's a significant discrepancy
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
	// FIX 5: Correct seconds calculation
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

	// FIX 2: Single-pass for BLER — multi-pass with cache defeat on every sector
	// is far too slow for a full-disc scan. BLER is meant to be a single-pass
	// error rate measurement like a real BLER analyzer.
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

			// FIX 3: Guard against unsigned underflow when lba < firstLBA
			size_t secIdx = 0;
			if (lba >= firstLBA) {
				secIdx = static_cast<size_t>((lba - firstLBA) / 75);
			}
			if (secIdx >= result.perSecondC2.size())
				secIdx = result.perSecondC2.size() - 1;

			if (m_drive.ReadSectorWithC2Ex(lba, buf.data(), nullptr, c2Errors, nullptr, c2Opts)) {
				if (c2Errors > 0) {
					result.totalC2Errors += c2Errors;
					result.totalC2Sectors++;
					result.perSecondC2[secIdx].first = static_cast<int>(firstLBA + secIdx * 75);
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
				// FIX 4: Use max C2 bytes per sector (296), not audio byte count (2352)
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
			result.worstSecondLBA = static_cast<int>(firstLBA + static_cast<DWORD>(i * 75));
		}
	}

	if (result.totalReadFailures > 0) result.qualityRating = "BAD";
	else if (result.totalC2Sectors == 0) result.qualityRating = "EXCELLENT";
	else if (result.avgC2PerSecond < 1.0 && result.consecutiveErrorSectors < 3) result.qualityRating = "GOOD";
	else if (result.avgC2PerSecond < 10.0 && result.consecutiveErrorSectors < 10) result.qualityRating = "ACCEPTABLE";
	else result.qualityRating = "POOR";

	// =========================================================================
	// DETAILED BLER SCAN REPORT
	// =========================================================================
	std::cout << "\n" << std::string(60, '=') << "\n";
	std::cout << "              BLER QUALITY SCAN REPORT\n";
	std::cout << std::string(60, '=') << "\n";

	std::cout << "\n--- Scan Configuration ---\n";
	std::cout << "  Speed:           " << (scanSpeed == 0 ? "Max" : std::to_string(scanSpeed) + "x") << "\n";
	std::cout << "  Sectors scanned: " << scannedSectors << "\n";
	std::cout << "  Disc length:     "
		<< (result.totalSeconds / 60) << ":" << std::setfill('0') << std::setw(2) << (result.totalSeconds % 60)
		<< std::setfill(' ') << " (mm:ss)\n";

	// Red Book compliance
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

	// Overall statistics
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

	// Per-track error summary
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
			DWORD secLBA = firstLBA + static_cast<DWORD>(i * 75);
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

	// Error distribution graph
	PrintBlerGraph(result);

	// Quality verdict
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


// ============================================================================
// Drive Capabilities
// ============================================================================

bool AudioCDCopier::DetectDriveCapabilities(DriveCapabilities& caps) {
	caps = DriveCapabilities{};

	std::string vendor, model;
	if (m_drive.GetDriveInfo(vendor, model)) {
		caps.vendor = vendor;
		caps.model = model;
	}

	caps.supportsC2ErrorReporting = m_drive.CheckC2Support();

	BYTE cdb[10] = { 0x5A, 0, 0x2A, 0, 0, 0, 0, 0, 28, 0 };
	std::vector<BYTE> buf(28);

	if (m_drive.SendSCSI(cdb, 10, buf.data(), 28)) {
		if (buf.size() >= 20) {
			caps.supportsAccurateStream = (buf[12] & 0x02) != 0;
			caps.supportsCDText = (buf[12] & 0x01) != 0;
			caps.supportsSubchannelRaw = true;

			if (buf.size() >= 22) {
				caps.maxReadSpeedKB = (buf[14] << 8) | buf[15];
				caps.currentReadSpeedKB = (buf[20] << 8) | buf[21];
			}
		}
	}

	return true;
}

void AudioCDCopier::PrintDriveCapabilities(const DriveCapabilities& caps) {
	std::cout << "\n=== Drive Capabilities ===\n";
	std::cout << "Drive: " << caps.vendor << " " << caps.model << "\n";
	std::cout << "C2 Error Reporting: " << (caps.supportsC2ErrorReporting ? "YES" : "NO") << "\n";
	std::cout << "Accurate Stream: " << (caps.supportsAccurateStream ? "YES" : "NO") << "\n";
	std::cout << "CD-TEXT Reading: " << (caps.supportsCDText ? "YES" : "NO") << "\n";
	std::cout << "Subchannel Reading: " << (caps.supportsSubchannelRaw ? "YES" : "NO") << "\n";
	if (caps.maxReadSpeedKB > 0) {
		std::cout << "Max Read Speed: " << caps.maxReadSpeedKB << " KB/s (" << caps.maxReadSpeedKB / 176 << "x)\n";
	}

	int qualityScore = 50;
	if (caps.supportsC2ErrorReporting) qualityScore += 20;
	if (caps.supportsAccurateStream) qualityScore += 15;
	if (caps.supportsCDText) qualityScore += 5;
	if (caps.supportsSubchannelRaw) qualityScore += 10;

	std::cout << "\nRipping Quality Score: " << qualityScore << "/100\n";

	if (qualityScore >= 80) std::cout << "Rating: EXCELLENT - Ideal for accurate ripping\n";
	else if (qualityScore >= 60) std::cout << "Rating: GOOD - Suitable for most ripping tasks\n";
	else std::cout << "Rating: BASIC - May have limitations for damaged discs\n";
}

// ============================================================================
// C2 Validation
// ============================================================================

bool AudioCDCopier::ValidateC2Accuracy(DWORD testLBA) {
	return m_drive.ValidateC2Accuracy(testLBA);
}

// ============================================================================
// Missing BLER and Disc Rot Helper Functions
// ============================================================================



void AudioCDCopier::PrintBlerGraph(const BlerResult& result, int width, int height) {
	if (result.perSecondC2.empty() || width <= 0 || height <= 0) return;

	std::cout << "\n=== C2 Error Distribution ===\n";
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

	for (int row = height; row > 0; row--) {
		int threshold = (maxC2 * row) / height;
		std::cout << std::setw(6) << threshold << " |";
		for (int col = 0; col < width; col++) {
			std::cout << (buckets[col] >= threshold ? '#' : ' ');
		}
		std::cout << "\n";
	}
	std::cout << "       +";
	for (int i = 0; i < width; i++) std::cout << '-';
	std::cout << "\n";
}

void AudioCDCopier::ClassifyZone(DWORD lba, DWORD firstLBA, DWORD lastLBA,
	int c2Errors, DiscZoneStats& zones) {
	DWORD range = lastLBA - firstLBA;
	DWORD pos = lba - firstLBA;
	double pct = range > 0 ? static_cast<double>(pos) / range : 0;

	if (pct < 0.33) {
		zones.innerSectors++;
		if (c2Errors > 0) zones.innerErrors++;
	}
	else if (pct < 0.66) {
		zones.middleSectors++;
		if (c2Errors > 0) zones.middleErrors++;
	}
	else {
		zones.outerSectors++;
		if (c2Errors > 0) zones.outerErrors++;
	}
}

void AudioCDCopier::DetectErrorClusters(const std::vector<DWORD>& errorLBAs,
	std::vector<ErrorCluster>& clusters) {
	if (errorLBAs.empty()) return;

	const int CLUSTER_GAP = 75;
	ErrorCluster current = { errorLBAs[0], errorLBAs[0], 1 };

	for (size_t i = 1; i < errorLBAs.size(); i++) {
		if (errorLBAs[i] - current.endLBA <= CLUSTER_GAP) {
			current.endLBA = errorLBAs[i];
			current.errorCount++;
		}
		else {
			if (current.errorCount >= 3) {
				clusters.push_back(current);
			}
			current = { errorLBAs[i], errorLBAs[i], 1 };
		}
	}
	if (current.errorCount >= 3) {
		clusters.push_back(current);
	}
}

bool AudioCDCopier::TestReadConsistency(DWORD lba, int passes, int& inconsistentCount) {
	std::vector<std::vector<BYTE>> reads(passes);
	inconsistentCount = 0;

	for (int i = 0; i < passes; i++) {
		// Defeat drive cache between reads so each pass is a fresh disc read
		if (i > 0) {
			DefeatDriveCache(lba, 0);
		}

		reads[i].resize(AUDIO_SECTOR_SIZE);
		if (!m_drive.ReadSectorAudioOnly(lba, reads[i].data())) {
			return false;
		}
	}

	for (int i = 1; i < passes; i++) {
		if (memcmp(reads[0].data(), reads[i].data(), AUDIO_SECTOR_SIZE) != 0) {
			inconsistentCount++;
		}
	}

	return true;
}

void AudioCDCopier::AnalyzeErrorPatterns(const std::vector<DWORD>& errorLBAs,
	DiscRotAnalysis& result) {
	auto& z = result.zones;

	double innerRate = z.InnerErrorRate();
	double middleRate = z.MiddleErrorRate();
	double outerRate = z.OuterErrorRate();

	if ((innerRate > middleRate * 2 && innerRate > 1.0) ||
		(outerRate > middleRate * 2 && outerRate > 1.0)) {
		result.edgeConcentration = true;
	}

	if (outerRate > middleRate * 1.5 && middleRate > innerRate * 1.5) {
		result.progressivePattern = true;
	}

	if (result.inconsistencyRate > 1.0) {
		result.readInstability = true;
	}

	int smallClusters = 0;
	for (const auto& c : result.clusters) {
		if (c.size() < 75) smallClusters++;
	}
	if (smallClusters > 5) {
		result.pinholePattern = true;
	}
}

std::string AudioCDCopier::AssessRotRisk(const DiscRotAnalysis& result) {
	int riskScore = 0;

	const auto& z = result.zones;
	double maxZoneRate = std::max({ z.InnerErrorRate(), z.MiddleErrorRate(), z.OuterErrorRate() });

	if (maxZoneRate > 5.0) riskScore += 3;
	else if (maxZoneRate > 1.0) riskScore += 2;
	else if (maxZoneRate > 0.1) riskScore += 1;

	if (result.edgeConcentration) riskScore += 2;
	if (result.progressivePattern) riskScore += 2;
	if (result.pinholePattern) riskScore += 1;
	if (result.readInstability) riskScore += 3;

	if (result.clusters.size() > 10) riskScore += 2;
	else if (result.clusters.size() > 3) riskScore += 1;

	if (riskScore == 0) return "NONE";
	else if (riskScore <= 2) return "LOW";
	else if (riskScore <= 4) return "MODERATE";
	else if (riskScore <= 6) return "HIGH";
	else return "CRITICAL";
}

void AudioCDCopier::PrintDiscRotReport(const DiscRotAnalysis& result) {
	std::cout << "\n" << std::string(60, '=') << "\n";
	std::cout << "            DISC ROT ANALYSIS REPORT\n";
	std::cout << std::string(60, '=') << "\n";

	// Zone error distribution with visual bars
	std::cout << "\n--- Zone Error Distribution ---\n";
	std::cout << std::fixed << std::setprecision(2);

	double innerRate = result.zones.InnerErrorRate();
	double middleRate = result.zones.MiddleErrorRate();
	double outerRate = result.zones.OuterErrorRate();
	double maxRate = std::max({ innerRate, middleRate, outerRate, 0.01 });

	auto drawBar = [&](double rate) {
		int len = static_cast<int>(rate / maxRate * 20);
		for (int i = 0; i < len; i++) std::cout << '#';
		for (int i = len; i < 20; i++) std::cout << ' ';
	};

	std::cout << "  Inner  (0-33%):   ";
	drawBar(innerRate);
	std::cout << " " << std::setw(6) << innerRate << "%  (" << result.zones.innerErrors << "/" << result.zones.innerSectors << ")\n";

	std::cout << "  Middle (33-66%):  ";
	drawBar(middleRate);
	std::cout << " " << std::setw(6) << middleRate << "%  (" << result.zones.middleErrors << "/" << result.zones.middleSectors << ")\n";

	std::cout << "  Outer  (66-100%): ";
	drawBar(outerRate);
	std::cout << " " << std::setw(6) << outerRate << "%  (" << result.zones.outerErrors << "/" << result.zones.outerSectors << ")\n";

	// Zone interpretation
	if (outerRate > innerRate * 2 && outerRate > 0.5) {
		std::cout << "  >> Outer edge has significantly more errors (typical of disc rot)\n";
	}
	else if (innerRate > outerRate * 2 && innerRate > 0.5) {
		std::cout << "  >> Inner hub area has more errors (possible hub cracking)\n";
	}

	// Read consistency
	std::cout << "\n--- Read Consistency ---\n";
	std::cout << "  Sectors tested:       " << result.totalRereadTests << "\n";
	std::cout << "  Inconsistent reads:   " << result.inconsistentSectors;
	if (result.totalRereadTests > 0) {
		std::cout << " (" << std::setprecision(1) << result.inconsistencyRate << "%)";
	}
	std::cout << "\n";
	if (result.inconsistencyRate > 5.0)
		std::cout << "  >> HIGH instability: disc surface is deteriorating\n";
	else if (result.inconsistencyRate > 1.0)
		std::cout << "  >> Moderate instability: early signs of degradation\n";
	else if (result.totalRereadTests > 0)
		std::cout << "  >> Reads are consistent\n";

	// Degradation pattern analysis
	std::cout << "\n--- Degradation Pattern Analysis ---\n";
	int patternsDetected = 0;

	std::cout << "  Edge concentration:     ";
	if (result.edgeConcentration) {
		std::cout << "YES - Errors cluster at disc edges\n";
		std::cout << "                            Typical cause: oxidation of reflective layer\n";
		patternsDetected++;
	}
	else std::cout << "No\n";

	std::cout << "  Progressive degradation:";
	if (result.progressivePattern) {
		std::cout << " YES - Error rate increases toward outer edge\n";
		std::cout << "                            Typical cause: spreading chemical breakdown\n";
		patternsDetected++;
	}
	else std::cout << " No\n";

	std::cout << "  Pinhole pattern:        ";
	if (result.pinholePattern) {
		std::cout << "YES - Scattered small damage clusters\n";
		std::cout << "                            Typical cause: micro-pitting of lacquer layer\n";
		patternsDetected++;
	}
	else std::cout << "No\n";

	std::cout << "  Read instability:       ";
	if (result.readInstability) {
		std::cout << "YES - Sectors return different data on re-read\n";
		std::cout << "                            Typical cause: advanced surface degradation\n";
		patternsDetected++;
	}
	else std::cout << "No\n";

	if (patternsDetected == 0)
		std::cout << "\n  No degradation patterns detected.\n";

	// Error clusters
	std::cout << "\n--- Error Clusters ---\n";
	if (result.clusters.empty()) {
		std::cout << "  No significant error clusters found.\n";
	}
	else {
		std::cout << "  Total clusters:   " << result.clusters.size() << "\n";

		int largest = 0;
		DWORD largestStart = 0;
		for (const auto& c : result.clusters) {
			if (c.size() > largest) {
				largest = c.size();
				largestStart = c.startLBA;
			}
		}
		std::cout << "  Largest cluster:  " << largest << " sectors (starting at LBA " << largestStart << ")\n";

		// Show up to 8 clusters
		int shown = 0;
		std::cout << "\n  Start LBA    End LBA    Size      Errors   Density\n";
		std::cout << "  " << std::string(55, '-') << "\n";
		for (const auto& c : result.clusters) {
			if (shown++ >= 8) {
				std::cout << "  ... and " << (result.clusters.size() - 8) << " more clusters\n";
				break;
			}
			double density = c.size() > 0 ? static_cast<double>(c.errorCount) / c.size() * 100.0 : 0;
			std::cout << "  " << std::setw(9) << c.startLBA
				<< "  " << std::setw(9) << c.endLBA
				<< "  " << std::setw(5) << c.size() << " sec"
				<< "  " << std::setw(5) << c.errorCount
				<< "    " << std::fixed << std::setprecision(0) << std::setw(3) << density << "%\n";
		}
	}

	// Risk assessment
	std::cout << "\n" << std::string(60, '=') << "\n";
	std::cout << "  DISC ROT RISK: ";

	if (result.rotRiskLevel == "NONE") {
		std::cout << "NONE";
		std::cout << "\n  No signs of physical deterioration detected.\n";
	}
	else if (result.rotRiskLevel == "LOW") {
		std::cout << "LOW [!]";
		std::cout << "\n  Minor anomalies found. Disc is still in good condition.\n";
	}
	else if (result.rotRiskLevel == "MODERATE") {
		std::cout << "MODERATE [!!]";
		std::cout << "\n  Early degradation signs present. Damage may progress over time.\n";
	}
	else if (result.rotRiskLevel == "HIGH") {
		std::cout << "HIGH [!!!]";
		std::cout << "\n  Significant degradation detected. Data loss is likely without action.\n";
	}
	else {
		std::cout << "CRITICAL [!!!!]";
		std::cout << "\n  Severe disc damage. Immediate action required.\n";
	}

	std::cout << "\n  >> " << result.recommendation << "\n";
	std::cout << std::string(60, '=') << "\n";
}

bool AudioCDCopier::SaveDiscRotLog(const DiscRotAnalysis& result, const std::wstring& filename) {
	std::ofstream log(filename);
	if (!log) return false;

	log << "DISC ROT ANALYSIS REPORT\n";
	log << "========================\n\n";
	log << "Risk Level: " << result.rotRiskLevel << "\n";
	log << "Recommendation: " << result.recommendation << "\n\n";

	log << "Zone Analysis:\n";
	log << std::fixed << std::setprecision(2);
	log << "  Inner (0-33%):   " << result.zones.InnerErrorRate() << "% errors\n";
	log << "  Middle (33-66%): " << result.zones.MiddleErrorRate() << "% errors\n";
	log << "  Outer (66-100%): " << result.zones.OuterErrorRate() << "% errors\n\n";

	log << "Read Consistency:\n";
	log << "  Tests: " << result.totalRereadTests << "\n";
	log << "  Inconsistent: " << result.inconsistentSectors << " (" << result.inconsistencyRate << "%)\n\n";

	log << "Patterns Detected:\n";
	log << "  Edge concentration: " << (result.edgeConcentration ? "Yes" : "No") << "\n";
	log << "  Progressive pattern: " << (result.progressivePattern ? "Yes" : "No") << "\n";
	log << "  Pinhole pattern: " << (result.pinholePattern ? "Yes" : "No") << "\n";
	log << "  Read instability: " << (result.readInstability ? "Yes" : "No") << "\n\n";

	log << "Error Clusters (" << result.clusters.size() << " total):\n";
	for (const auto& c : result.clusters) {
		log << "  LBA " << c.startLBA << "-" << c.endLBA
			<< " (" << c.size() << " sectors)\n";
	}

	log.close();
	return true;
}