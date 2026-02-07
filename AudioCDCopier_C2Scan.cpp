#define NOMINMAX
#include "AudioCDCopier.h"
#include "InterruptHandler.h"
#include <iostream>
#include <iomanip>
#include <vector>
#include <algorithm>

// ============================================================================
// C2 Error Scanning
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

bool AudioCDCopier::ValidateC2Accuracy(DWORD testLBA) {
	return m_drive.ValidateC2Accuracy(testLBA);
}