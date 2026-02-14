// ============================================================================
// main.cpp - Application entry point and interactive menu loop
//
// Initialises the console, discovers CD/DVD drives, reads disc metadata
// (TOC, CD-TEXT, ISRC, AccurateRip), and presents a 19-item menu for
// copying, scanning, and analysing audio CDs.
// ============================================================================
#define NOMINMAX  // Prevent Windows.h min/max macros from colliding with <algorithm>

#include "AudioCDCopier.h"
#include "AccurateRip.h"
#include "InterruptHandler.h"
#include "MenuHelpers.h"
#include "Progress.h"
#include "ConsoleColors.h"
#include "Drive.h"
#include "FileUtils.h"
#include "MenuUI.h"
#include "CopyWorkflow.h"
#include <windows.h>
#include <iostream>

// ── Helper: multi-drive selection UI ────────────────────────────────────────
// Displays a numbered list of drives that contain audio CDs and returns the
// drive letter the user picked.  Extracted to eliminate duplication between
// initial startup and the "Rescan disc" menu item.
wchar_t SelectAudioDrive(const std::vector<wchar_t>& audioDrives) {
	Console::Warning("\nMultiple audio CDs detected. Select drive:\n");
	for (size_t i = 0; i < audioDrives.size(); i++) {
		HANDLE h = OpenDriveHandle(audioDrives[i]);
		std::string name = (h != INVALID_HANDLE_VALUE) ? GetDriveName(h) : "CD/DVD drive";
		int tracks = (h != INVALID_HANDLE_VALUE) ? GetAudioTrackCount(h) : 0;
		if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
		std::cout << "  " << (i + 1) << ". [";
		Console::SetColor(Console::Color::Yellow);
		std::cout << static_cast<char>(audioDrives[i]) << ":";
		Console::Reset();
		std::cout << "] " << name;
		if (tracks > 0) std::cout << " (" << tracks << " tracks)";
		std::cout << "\n";
	}
	std::cout << "Choice: ";
	int pick = GetMenuChoice(1, static_cast<int>(audioDrives.size()), 1);
	std::cin.clear();
	if (std::cin.peek() == '\n') std::cin.ignore();
	return audioDrives[pick - 1];
}

int main() {
	// ── Console initialisation ──────────────────────────────────────────────
	// Enable ANSI/VT100 escape sequences for coloured output in modern
	// Windows terminals, and set the output code page to UTF-8.
	HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
	if (hOut != INVALID_HANDLE_VALUE && hOut != NULL) {
		DWORD mode = 0;
		if (GetConsoleMode(hOut, &mode)) {
			SetConsoleMode(hOut, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
		}
	}
	SetConsoleOutputCP(CP_UTF8);

	// Apply a custom console appearance: Cascadia Mono font at 20pt,
	// 110×38 character window, and a dark colour scheme.
	Console::SetFont(L"Cascadia Mono", 20);
	Console::SetWindowSize(110, 38);
	Console::ApplyDarkTheme();

	// Register a Ctrl+C / Ctrl+Break handler for graceful interruption of
	// long-running operations (e.g. disc scans).
	g_interrupt.Install();

	// Centre the console window on the primary monitor and resolve the
	// working directory where output files (WAV, FLAC, logs) will be saved.
	CenterConsoleWindow();
	std::wstring dir = GetWorkingDirectory();
	SetCurrentDirectoryW(dir.c_str());

	// Print the application banner with working-directory info and exit help.
	Console::BoxHeading("Audio CD Copy Tool");
	std::cout << Console::Sym::Vertical << " Working directory: ";
	std::wcout << dir << L"\n";
	InterruptHandler::PrintExitHelp();
	Console::BoxFooter();

	// ── Drive discovery ─────────────────────────────────────────────────────
	// Enumerate all CD/DVD drives on the system.  Drives that already contain
	// an audio CD are collected into `audioDrives`.
	Console::Info("Scanning drives...\n");
	wchar_t audioDrive = 0;
	std::vector<wchar_t> audioDrives;
	std::vector<wchar_t> cdDrives = ScanDrives(audioDrives);

	if (cdDrives.empty()) {
		Console::Error("No CD/DVD drives found!\n");
		return 1;
	}

	// Auto-select if exactly one audio disc is present.  If multiple are
	// found, display a numbered list and let the user choose.
	if (audioDrives.size() == 1) {
		audioDrive = audioDrives[0];
	}
	else if (audioDrives.size() > 1) {
		audioDrive = SelectAudioDrive(audioDrives);
	}

	// No audio disc found at startup — poll drives until the user inserts one.
	if (!audioDrive) {
		audioDrive = WaitForDisc(cdDrives, 0);
		if (!audioDrive) {
			// No audio disc inserted — let the user pick any available drive
			// so drive-level features (capabilities, write speeds) still work.
			if (cdDrives.size() == 1) {
				audioDrive = cdDrives[0];
				Console::Warning("No audio disc detected. Using drive ");
				Console::SetColor(Console::Color::Yellow);
				std::cout << static_cast<char>(audioDrive) << ":";
				Console::Reset();
				std::cout << " for drive-level operations.\n";
			}
			else if (cdDrives.size() > 1) {
				Console::Warning("No audio disc detected. Select a drive for drive-level operations:\n");
				for (size_t i = 0; i < cdDrives.size(); i++) {
					std::cout << "  " << (i + 1) << ". [";
					Console::SetColor(Console::Color::Yellow);
					std::cout << static_cast<char>(cdDrives[i]) << ":";
					Console::Reset();
					std::cout << "]\n";
				}
				std::cout << "Choice: ";
				int pick = GetMenuChoice(1, static_cast<int>(cdDrives.size()), 1);
				std::cin.clear();
				if (std::cin.peek() == '\n') std::cin.ignore();
				audioDrive = cdDrives[pick - 1];
			}
			else {
				Console::Error("No disc selected.\n");
				return 1;
			}
		}
	}

	// Confirm the selected drive letter.
	std::cout << "\nUsing drive ";
	Console::SetColor(Console::Color::Yellow);
	std::cout << static_cast<char>(audioDrive) << ":";
	Console::Reset();
	std::cout << "\n";

	// ── Open drive and read disc metadata ───────────────────────────────────
	// Open a raw SCSI pass-through handle to the drive, read the Table of
	// Contents, then attempt to read optional metadata: CD-TEXT (album/track
	// titles), ISRC codes, and AccurateRip verification CRCs.
	AudioCDCopier copier;
	if (!copier.Open(audioDrive)) {
		Console::Error("Failed to open drive\n");
		return 1;
	}

	DiscInfo disc;
	bool hasTOC = copier.ReadTOC(disc);
	if (!hasTOC) {
		Console::Warning("No TOC found (empty or blank disc). Disc-dependent features will be unavailable.\n");
	}

	if (hasTOC) {
		copier.ReadCDText(disc);       // Read embedded CD-TEXT metadata (artist/title)
		copier.ReadISRC(disc);         // Read per-track ISRC catalogue codes
		std::vector<std::vector<uint32_t>> pressingCRCs;
		AccurateRip::Lookup(disc, pressingCRCs);  // Query the AccurateRip online database
		PrintDiscInfo(disc);           // Display a summary of what was found
	}

	// ── Main interactive menu loop ──────────────────────────────────────────
	// Each iteration prints the 19-item menu (grouped by category), reads
	// the user's choice, and dispatches to the corresponding operation.
	// Loops until "Exit".
	while (true) {
		Console::BoxHeading("Operation");

		// ── Ripping ─────────────────────────────────────────────────────
		PrintMenuSection("Ripping");
		PrintMenuItem(1, "Copy disc");

		// ── Disc Quality ────────────────────────────────────────────────
		PrintMenuSection("Disc Quality");
		PrintMenuItem(2, "C2 error scan");
		PrintMenuItem(3, "BLER scan (detailed)");
		PrintMenuItem(4, "Disc rot detection");
		PrintMenuItem(5, "Generate surface map");
		PrintMenuItem(6, "Multi-pass verification");

		// ── Disc Information ────────────────────────────────────────────
		PrintMenuSection("Disc Info");
		PrintMenuItem(7, "Audio content analysis");
		PrintMenuItem(8, "Disc fingerprint (CDDB/MusicBrainz/AccurateRip IDs)");
		PrintMenuItem(9, "Lead area check");
		PrintMenuItem(10, "Subchannel integrity check");
		PrintMenuItem(11, "Verify subchannel burn status");

		// ── Drive Diagnostics ───────────────────────────────────────────
		PrintMenuSection("Drive");
		PrintMenuItem(12, "Drive capabilities");
		PrintMenuItem(13, "Drive offset detection");
		PrintMenuItem(14, "C2 validation test");
		PrintMenuItem(15, "Speed comparison test");
		PrintMenuItem(16, "Seek time analysis");

		// ── Utility ─────────────────────────────────────────────────────
		PrintMenuSection("Utility");
		PrintMenuItem(17, "Rescan disc");
		PrintMenuItem(18, "Help (test descriptions)");
		PrintMenuItem(19, "Exit", true);  // Dimmed style for the exit item

		Console::BoxFooter();
		std::cout << Console::Sym::Arrow << " Choice: ";

		int choice = GetMenuChoice(1, 19, 1);
		std::cin.clear();
		if (std::cin.peek() == '\n') {
			std::cin.ignore();  // Consume trailing newline left by GetMenuChoice
		}

		switch (choice) {

			// ════════════════════════════════════════════════════════════
			//  Ripping
			// ════════════════════════════════════════════════════════════

			// ── 1. Copy disc ────────────────────────────────────────────
			// Full copy workflow: format selection, offset correction,
			// secure rip mode, WAV/FLAC encoding, AccurateRip verification.
		case 1:
			if (!hasTOC) { Console::Error("This operation requires a disc with a valid TOC.\n"); break; }
			RunCopyWorkflow(copier, disc, dir);
			break;

			// ════════════════════════════════════════════════════════════
			//  Disc Quality
			// ════════════════════════════════════════════════════════════

			// ── 2. C2 error scan ────────────────────────────────────────
			// Quick disc health check using the drive's C2 error reporting.
		case 2: {
			if (!hasTOC) { Console::Error("This operation requires a disc with a valid TOC.\n"); break; }
			int speed = copier.SelectScanSpeed();
			if (speed == -1) break;
			BlerResult c2Result;
			if (copier.RunC2Scan(disc, c2Result, speed)) {
				std::wstring logPath = dir + L"\\c2_scan.csv";
				if (copier.SaveBlerLog(c2Result, logPath)) {
					Console::Success("C2 scan log saved to: ");
					std::wcout << logPath << L"\n";
				}
			}
			else {
				Console::Error("C2 scan failed.\n");
			}
			break;
		}

			  // ── 3. BLER scan (detailed) ─────────────────────────────────
			  // Block Error Rate scan — measures raw error frequency before ECC.
		case 3: {
			if (!hasTOC) { Console::Error("This operation requires a disc with a valid TOC.\n"); break; }
			int speed = copier.SelectScanSpeed();
			if (speed == -1) break;
			BlerResult result;
			if (copier.RunBlerScan(disc, result, speed)) {
				std::wstring logPath = dir + L"\\bler_scan.csv";
				if (copier.SaveBlerLog(result, logPath)) {
					Console::Success("BLER log saved to: ");
					std::wcout << logPath << L"\n";
				}
			}
			else {
				Console::Error("BLER scan failed.\n");
			}
			break;
		}

			  // ── 4. Disc rot detection ───────────────────────────────────
			  // Analyses error patterns for signs of physical degradation.
		case 4: {
			if (!hasTOC) { Console::Error("This operation requires a disc with a valid TOC.\n"); break; }
			int speed = copier.SelectScanSpeed();
			if (speed == -1) break;
			DiscRotAnalysis result;
			if (copier.RunDiscRotScan(disc, result, speed)) {
				std::wstring logPath = dir + L"\\discrot_report.txt";
				if (copier.SaveDiscRotLog(result, logPath)) {
					Console::Success("Disc rot report saved to: ");
					std::wcout << logPath << L"\n";
				}
			}
			else {
				Console::Error("Disc rot scan failed.\n");
			}
			break;
		}

			  // ── 5. Generate surface map ─────────────────────────────────
			  // CSV-based detailed map of disc surface quality.
		case 5: {
			if (!hasTOC) { Console::Error("This operation requires a disc with a valid TOC.\n"); break; }
			int speed = copier.SelectScanSpeed();
			if (speed == -1) break;
			std::wstring mapFile = dir + L"\\surface_map.csv";
			copier.GenerateSurfaceMap(disc, mapFile, speed);
			break;
		}

			  // ── 6. Multi-pass verification ──────────────────────────────
			  // Reads entire disc N times and compares for consistency.
		case 6: {
			if (!hasTOC) { Console::Error("This operation requires a disc with a valid TOC.\n"); break; }
			int speed = copier.SelectScanSpeed();
			if (speed == -1) break;
			std::cout << "\n=== Multi-Pass Verification ===\n";
			std::cout << "Select number of passes (2-10, recommended: 3): ";
			int passes = GetMenuChoice(2, 10, 3);
			std::vector<MultiPassResult> results;
			copier.RunMultiPassVerification(disc, results, passes, speed);
			break;
		}

			  // ════════════════════════════════════════════════════════════
			  //  Disc Information
			  // ════════════════════════════════════════════════════════════

				// ── 7. Audio content analysis ───────────────────────────────
				// Silence, clipping, dynamic range, pre-emphasis, HDCD.
		case 7: {
			if (!hasTOC) { Console::Error("This operation requires a disc with a valid TOC.\n"); break; }
			int speed = copier.SelectScanSpeed();
			if (speed == -1) break;
			AudioAnalysisResult result;
			copier.AnalyzeAudioContent(disc, result, speed);
			break;
		}

			  // ── 8. Disc fingerprint ────────────────────────────────────
			  // CDDB, MusicBrainz, AccurateRip IDs, audio content hash.
		case 8: {
			if (!hasTOC) { Console::Error("This operation requires a disc with a valid TOC.\n"); break; }
			DiscFingerprint fingerprint;
			if (copier.GenerateDiscFingerprint(disc, fingerprint)) {
				copier.PrintDiscFingerprint(fingerprint);
				std::wstring fpPath = dir + L"\\disc_fingerprint.txt";
				if (copier.SaveDiscFingerprint(fingerprint, fpPath)) {
					Console::Success("Fingerprint saved to: ");
					std::wcout << fpPath << L"\n";
				}
			}
			else {
				Console::Error("Failed to generate disc fingerprint.\n");
			}
			break;
		}

			  // ── 9. Lead area check ─────────────────────────────────────
			  // Examines lead-in/lead-out for hidden data or damage.
		case 9: {
			if (!hasTOC) { Console::Error("This operation requires a disc with a valid TOC.\n"); break; }
			int speed = copier.SelectScanSpeed();
			if (speed == -1) break;
			copier.CheckLeadAreas(disc, speed);
			break;
		}

			  // ── 10. Subchannel integrity check ─────────────────────────
			  // Q-channel subchannel data across all sectors.
		case 10: {
			if (!hasTOC) { Console::Error("This operation requires a disc with a valid TOC.\n"); break; }
			int speed = copier.SelectScanSpeed();
			if (speed == -1) break;
			int errorCount = 0;
			Console::Info("\nChecking subchannel integrity...\n");
			if (copier.VerifySubchannelIntegrity(disc, errorCount, speed)) {
				if (errorCount == 0) {
					Console::Success("Subchannel data integrity verified - no errors found.\n");
				}
				else {
					Console::Warning("Subchannel errors detected: ");
					std::cout << errorCount << " issues found.\n";
				}
			}
			else {
				Console::Error("Failed to verify subchannel integrity.\n");
			}
			break;
		}

			   // ── 11. Verify subchannel burn status ──────────────────────
			   // Inspects raw subchannel data to determine whether it was
			   // genuinely mastered/burned onto the disc.
		case 11: {
			if (!hasTOC) { Console::Error("This operation requires a disc with a valid TOC.\n"); break; }
			int speed = copier.SelectScanSpeed();
			if (speed == -1) break;
			SubchannelBurnResult burnResult;
			Console::Info("\nVerifying subchannel burn status...\n");
			if (!copier.VerifySubchannelBurnStatus(disc, burnResult, speed)) {
				Console::Error("Failed to verify subchannel burn status.\n");
			}
			break;
		}

			   // ════════════════════════════════════════════════════════════
			   //  Drive Diagnostics
			   // ════════════════════════════════════════════════════════════

				  // ── 12. Drive capabilities ─────────────────────────────────
				  // SCSI feature pages, hardware capabilities, ripping score.
		case 12: {
			DriveCapabilities caps;
			if (copier.DetectDriveCapabilities(caps)) {
				copier.PrintDriveCapabilities(caps);
			}
			else {
				Console::Error("Failed to detect drive capabilities.\n");
			}
			break;
		}

			   // ── 13. Drive offset detection ─────────────────────────────
			   // Auto-detect sample read offset via AccurateRip database.
		case 13: {
			OffsetDetectionResult offsetResult;
			Console::Info("\nDetecting drive read offset...\n");
			if (copier.DetectDriveOffset(offsetResult)) {
				Console::Success("Offset detected: ");
				std::cout << offsetResult.offset << " samples";
				std::cout << " (confidence: " << offsetResult.confidence << "%)\n";
				std::cout << "Method: " << offsetResult.details << "\n";
			}
			else {
				Console::Warning("Could not auto-detect offset.\n");
				Console::Info("Recommendation: Use a test disc or lookup at accuraterip.com/driveoffsets.htm\n");
			}
			break;
		}

			   // ── 14. C2 validation test ─────────────────────────────────
			   // Reads same sectors at different speeds, compares C2 results.
		case 14: {
			Console::Info("\n=== C2 Validation Test ===\n");
			Console::Info("This test reads sectors at different speeds to verify C2 accuracy.\n");
			Console::Info("Inconsistent C2 results may indicate unreliable C2 reporting.\n\n");

			// Select up to 3 mid-track LBAs spread across the disc to cover
			// inner, middle, and outer regions.
			std::vector<DWORD> testLBAs;
			for (const auto& t : disc.tracks) {
				if (!t.isAudio) continue;
				DWORD mid = t.startLBA + (t.endLBA - t.startLBA) / 2;
				testLBAs.push_back(mid);
				if (testLBAs.size() >= 3) break;
			}
			if (testLBAs.empty()) {
				Console::Warning("No audio tracks found.\n");
				break;
			}

			ProgressIndicator prog;
			prog.SetLabel("Validating C2");
			prog.Start();

			// Validate each test LBA independently.
			bool cancelled = false;
			int passed = 0;
			int total = static_cast<int>(testLBAs.size());
			for (int idx = 0; idx < total; idx++) {
				if (g_interrupt.IsInterrupted() || g_interrupt.CheckEscapeKey()) {
					Console::Warning("\n*** C2 validation cancelled by user ***\n");
					cancelled = true;
					break;
				}
				DWORD lba = testLBAs[idx];
				Console::Info("Testing LBA: ");
				std::cout << lba << "\n";
				if (copier.ValidateC2Accuracy(lba))
					passed++;
				prog.Update(idx + 1, total);
			}

			prog.Finish(!cancelled && passed == total);

			// Report overall pass/fail verdict.
			if (!cancelled) {
				if (passed == total) {
					Console::Success("\nC2 Validation: PASSED\n");
					Console::Success("Your drive's C2 error reporting appears reliable.\n");
					std::string infoMsg = "C2 pointers were consistent across " + std::to_string(testLBAs.size()) + " sectors and multiple speeds.\n";
					Console::Info(infoMsg.c_str());
				}
				else {
					std::string failMsg = "\nC2 Validation: FAILED (" + std::to_string(passed) + "/" + std::to_string(testLBAs.size()) + " sectors passed)\n";
					Console::Warning(failMsg.c_str());
					Console::Warning("Your drive's C2 error reporting may be unreliable.\n");
					Console::Warning("Consider using BLER scan instead for quality checks.\n");
				}
			}
			break;
		}

			   // ── 15. Speed comparison test ──────────────────────────────
			   // Reads sectors at several speeds, compares C2 error counts.
		case 15: {
			std::vector<SpeedComparisonResult> results;
			copier.RunSpeedComparisonTest(disc, results);
			break;
		}

			   // ── 16. Seek time analysis ─────────────────────────────────
			   // Measures mechanical seek latency at various disc positions.
		case 16: {
			std::vector<SeekTimeResult> results;
			Console::Info("\nRunning seek time analysis...\n");
			if (copier.RunSeekTimeAnalysis(disc, results)) {
				Console::Success("Seek time analysis complete.\n");
			}
			else {
				Console::Error("Seek time analysis failed.\n");
			}
			break;
		}

			   // ════════════════════════════════════════════════════════════
			   //  Utility
			   // ════════════════════════════════════════════════════════════

				  // ── 17. Rescan disc ────────────────────────────────────────
				  // Re-enumerates drives, re-opens handle, re-reads metadata.
		case 17: {
			Console::Info("\nScanning drives...\n");
			wchar_t newAudioDrive = 0;
			std::vector<wchar_t> newAudioDrives;
			std::vector<wchar_t> newCdDrives = ScanDrives(newAudioDrives);

			if (newCdDrives.empty()) {
				Console::Error("No CD/DVD drives found!\n");
				break;
			}

			// Same auto-select / multi-select logic as initial startup.
			if (newAudioDrives.size() == 1) {
				newAudioDrive = newAudioDrives[0];
			}
			else if (newAudioDrives.size() > 1) {
				newAudioDrive = SelectAudioDrive(newAudioDrives);
			}

			if (!newAudioDrive) {
				newAudioDrive = WaitForDisc(newCdDrives, 0);
				if (!newAudioDrive) {
					Console::Error("No disc selected.\n");
					break;
				}
			}

			// If the drive letter changed, close the old handle and open the
			// new one.  Falls back to the original drive on failure.
			if (newAudioDrive != audioDrive) {
				copier.Close();
				if (!copier.Open(newAudioDrive)) {
					Console::Error("Failed to open drive\n");
					// Try to reopen the original drive
					if (!copier.Open(audioDrive)) {
						Console::Error("Failed to reopen original drive\n");
						return 1;
					}
					break;
				}
				audioDrive = newAudioDrive;
				std::cout << "\nSwitched to drive ";
				Console::SetColor(Console::Color::Yellow);
				std::cout << static_cast<char>(audioDrive) << ":";
				Console::Reset();
				std::cout << "\n";
			}

			// Re-read all disc metadata from the newly inserted / selected disc.
			Console::Info("Rescanning disc...\n");
			disc = DiscInfo{};
			if (!copier.ReadTOC(disc)) {
				Console::Error("Failed to read TOC. Is a disc inserted?\n");
				break;
			}
			copier.ReadCDText(disc);
			copier.ReadISRC(disc);
			std::vector<std::vector<uint32_t>> pressingCRCs;
			AccurateRip::Lookup(disc, pressingCRCs);
			PrintDiscInfo(disc);
			Console::Success("Disc rescan complete.\n");
			break;
		}

			   // ── 18. Help ───────────────────────────────────────────────
			   // Prints detailed descriptions for every menu item.
		case 18:
			PrintHelpMenu();
			break;

			// ── 19. Exit ───────────────────────────────────────────────
		case 19:
			copier.Close();
			Console::Success("\nGoodbye!\n");
			return 0;

		default:
			Console::Warning("Unknown option.\n");
			break;
		}

		// Pause after each operation so the user can review the output before
		// the menu redraws.
		if (choice != 19) {
			WaitForKey();
		}
	}
}