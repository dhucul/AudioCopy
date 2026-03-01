#include "MainMenu.h"
#include "AccurateRip.h"
#include "CopyWorkflow.h"
#include "Drive.h"
#include "DriveOffsetDatabase.h"
#include "DriveSelection.h"
#include "InterruptHandler.h"
#include "MenuHelpers.h"
#include "MenuUI.h"
#include "Progress.h"
#include "ProtectionCheck.h"
#include <iostream>
#include <vector>

int RunMainMenuLoop(AudioCDCopier& copier, DiscInfo& disc, const std::wstring& workDir, wchar_t& audioDrive, bool& hasTOC) {
	while (true) {
		Console::BoxHeading("Operation");

		// ── Ripping ─────────────────────────────────────────────────────
		PrintMenuSection("Ripping");
		PrintMenuItem(1, "Copy disc");
		PrintMenuItem(2, "Write disc (.bin/.cue/.sub files)");

		// ── Disc Quality ────────────────────────────────────────────────
		PrintMenuSection("Disc Quality");
		PrintMenuItem(3, "C2 error scan");
		PrintMenuItem(4, "BLER scan (detailed)");
		PrintMenuItem(5, "Disc rot detection");
		PrintMenuItem(6, "Generate surface map");
		PrintMenuItem(7, "Multi-pass verification");

		// ── Disc Information ────────────────────────────────────────────
		PrintMenuSection("Disc Info");
		PrintMenuItem(8, "Audio content analysis");
		PrintMenuItem(9, "Disc fingerprint (CDDB/MusicBrainz/AccurateRip IDs)");
		PrintMenuItem(10, "Lead area check");
		PrintMenuItem(11, "Subchannel integrity check");
		PrintMenuItem(12, "Verify subchannel burn status");
		PrintMenuItem(13, "Copy-protection check");

		// ── Drive Diagnostics ───────────────────────────────────────────
		PrintMenuSection("Drive");
		PrintMenuItem(14, "Drive capabilities");
		PrintMenuItem(15, "Drive offset detection");
		PrintMenuItem(16, "C2 validation test");
		PrintMenuItem(17, "Speed comparison test");
		PrintMenuItem(18, "Seek time analysis");
		PrintMenuItem(19, "Quality scan (C1/C2/CU graphs)");
		PrintMenuItem(20, "Chipset identification");
		PrintMenuItem(21, "Disc balance check");

		// ── Utility ─────────────────────────────────────────────────────
		PrintMenuSection("Utility");
		PrintMenuItem(22, "Rescan disc");
		PrintMenuItem(23, "Help (test descriptions)");
		PrintMenuItem(24, "Exit", true);

		Console::BoxFooter();
		std::cout << Console::Sym::Arrow << " Choice: ";

		int choice = GetMenuChoice(1, 24, 1);
		std::cin.clear();
		if (std::cin.peek() == '\n') {
			std::cin.ignore();
		}

		switch (choice) {

			// ════════════════════════════════════════════════════════════
			//  Ripping
			// ════════════════════════════════════════════════════════════

			// ── 1. Copy disc ────────────────────────────────────────────
		case 1:
			if (!hasTOC) { Console::Error("This operation requires a disc with a valid TOC.\n"); break; }
			RunCopyWorkflow(copier, disc, workDir);
			break;

			// ── 2. Write disc ───────────────────────────────────────────
		case 2:
			RunWriteDiscWorkflow(copier, workDir);
			break;

			// ════════════════════════════════════════════════════════════
			//  Disc Quality
			// ════════════════════════════════════════════════════════════

			// ── 3. C2 error scan ────────────────────────────────────────
		case 3: {
			if (!hasTOC) { Console::Error("This operation requires a disc with a valid TOC.\n"); break; }
			int speed = copier.SelectScanSpeed();
			if (speed == -1) break;
			BlerResult c2Result;
			if (copier.RunC2Scan(disc, c2Result, speed)) {
				std::wstring logPath = workDir + L"\\c2_scan.csv";
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

			  // ── 4. BLER scan (detailed) ─────────────────────────────────
		case 4: {
			if (!hasTOC) { Console::Error("This operation requires a disc with a valid TOC.\n"); break; }
			int speed = copier.SelectScanSpeed();
			if (speed == -1) break;
			BlerResult result;
			if (copier.RunBlerScan(disc, result, speed)) {
				std::wstring logPath = workDir + L"\\bler_scan.csv";
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

			  // ── 5. Disc rot detection ───────────────────────────────────
		case 5: {
			if (!hasTOC) { Console::Error("This operation requires a disc with a valid TOC.\n"); break; }
			int speed = copier.SelectScanSpeed();
			if (speed == -1) break;
			DiscRotAnalysis result;
			if (copier.RunDiscRotScan(disc, result, speed)) {
				std::wstring logPath = workDir + L"\\discrot_report.txt";
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

			  // ── 6. Generate surface map ─────────────────────────────────
		case 6: {
			if (!hasTOC) { Console::Error("This operation requires a disc with a valid TOC.\n"); break; }
			int speed = copier.SelectScanSpeed();
			if (speed == -1) break;
			std::wstring mapFile = workDir + L"\\surface_map.csv";
			copier.GenerateSurfaceMap(disc, mapFile, speed);
			break;
		}

			  // ── 7. Multi-pass verification ──────────────────────────────
		case 7: {
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

				// ── 8. Audio content analysis ───────────────────────────────
		case 8: {
			if (!hasTOC) { Console::Error("This operation requires a disc with a valid TOC.\n"); break; }
			int speed = copier.SelectScanSpeed();
			if (speed == -1) break;
			AudioAnalysisResult result;
			copier.AnalyzeAudioContent(disc, result, speed);
			break;
		}

			  // ── 9. Disc fingerprint ────────────────────────────────────
		case 9: {
			if (!hasTOC) { Console::Error("This operation requires a disc with a valid TOC.\n"); break; }
			DiscFingerprint fingerprint;
			if (copier.GenerateDiscFingerprint(disc, fingerprint)) {
				copier.PrintDiscFingerprint(fingerprint);
				std::wstring fpPath = workDir + L"\\disc_fingerprint.txt";
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

			  // ── 10. Lead area check ────────────────────────────────────
		case 10: {
			if (!hasTOC) { Console::Error("This operation requires a disc with a valid TOC.\n"); break; }
			int speed = copier.SelectScanSpeed();
			if (speed == -1) break;
			copier.CheckLeadAreas(disc, speed);
			break;
		}

			   // ── 11. Subchannel integrity check ─────────────────────────
		case 11: {
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

			   // ── 12. Verify subchannel burn status ──────────────────────
		case 12: {
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

			   // ── 13. Copy-protection check ─────────────────────────────
		case 13: {
			if (!hasTOC) { Console::Error("This operation requires a disc with a valid TOC.\n"); break; }
			int speed = copier.SelectScanSpeed();
			if (speed == -1) break;
			RunProtectionCheck(copier, disc, workDir, speed);
			break;
		}

			   // ════════════════════════════════════════════════════════════
			   //  Drive Diagnostics
			   // ════════════════════════════════════════════════════════════

				  // ── 14. Drive capabilities ─────────────────────────────────
		case 14: {
			DriveCapabilities caps;
			if (copier.DetectDriveCapabilities(caps)) {
				copier.PrintDriveCapabilities(caps);
				std::cout << "\n";
				copier.ShowDriveRecommendations();
			}
			else {
				Console::Error("Failed to query drive capabilities.\n");
			}
			break;
		}

			   // ── 15. Drive offset detection ─────────────────────────────
		case 15: {
			OffsetDetectionResult offsetResult;
			Console::Info("\nDetecting drive read offset...\n");

			// Always ensure the full database is loaded
			auto& db = DriveOffsetDatabase::Instance();
			if (!db.IsLoaded() || db.Count() < 100) {
				Console::Info("Loading full drive offset database...\n");
				db.Refresh();
			}

			// Generate compiled header from downloaded data
			if (db.Count() > 100) {
				std::string headerPath = "C:\\Users\\dhucu\\source\\repos\\AudioCopy\\DriveOffsets.h";
				if (db.GenerateHeader(headerPath)) {
					Console::Success("DriveOffsets.h updated with ");
					std::cout << db.Count() << " drives. Rebuild to compile in.\n";
				}
			}

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

			   // ── 16. C2 validation test ─────────────────────────────────
		case 16: {
			Console::Info("\n=== C2 Validation Test ===\n");
			Console::Info("This test reads sectors at different speeds to verify C2 accuracy.\n");
			Console::Info("Inconsistent C2 results may indicate unreliable C2 reporting.\n\n");

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

			bool cancelled = false;
			int passed = 0;
			int total = static_cast<int>(testLBAs.size());
			for (int idx = 0; idx < total; idx++) {
				if (InterruptHandler::Instance().IsInterrupted() || InterruptHandler::Instance().CheckEscapeKey()) {
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

			   // ── 17. Speed comparison test ──────────────────────────────
		case 17: {
			std::vector<SpeedComparisonResult> results;
			copier.RunSpeedComparisonTest(disc, results);
			break;
		}

			   // ── 18. Seek time analysis ─────────────────────────────────
		case 18: {
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

			   // ── 19. Plextor Q-Check scan ───────────────────────────────
		case 19: {
			if (!hasTOC) { Console::Error("This operation requires a disc with a valid TOC.\n"); break; }
			int speed = copier.SelectScanSpeed();
			if (speed == -1) break;
			QCheckResult qcheckResult;
			if (copier.RunQCheckScan(disc, qcheckResult, speed)) {
				std::wstring logPath = workDir + L"\\qcheck_scan.csv";
				if (copier.SaveQCheckLog(qcheckResult, logPath)) {
					Console::Success("Q-Check scan log saved to: ");
					std::wcout << logPath << L"\n";
				}
			}
			else {
				if (!qcheckResult.supported) {
					Console::Warning("Q-Check is not available on this drive.\n");
					Console::Info("Q-Check requires a classic Plextor drive:\n");
					Console::Info("  PX-708A, PX-712A/SA, PX-716A/SA/AL, PX-755A/SA, PX-760A/SA\n");
					Console::Info("\nIf your drive supports D8 reads, use option 4 (BLER Scan) for\n");
					Console::Info("C2 error analysis. C1 block error rates are not measurable\n");
					Console::Info("without a Q-Check-capable drive.\n");
				}
				else {
					Console::Error("Q-Check scan failed.\n");
				}
			}
			break;
		}

			   // ── 20. Chipset identification ─────────────────────────────
		case 20: {
			Console::Info("\nIdentifying drive chipset / controller...\n");
			ChipsetInfo chipsetInfo;
			if (copier.DetectChipset(chipsetInfo)) {
				copier.PrintChipsetInfo(chipsetInfo);
			}
			else {
				Console::Error("Failed to identify drive chipset.\n");
			}
			break;
		}

			   // ── 21. Disc balance check ────────────────────────────────
		case 21: {
			if (!hasTOC) { Console::Error("This operation requires a disc with a valid TOC.\n"); break; }
			int balanceScore = 0;
			Console::Info("\nRunning disc balance check...\n");
			if (copier.CheckDiscBalance(disc, balanceScore)) {
				Console::Success("Disc balance check complete.\n");
			}
			else {
				Console::Error("Disc balance check failed.\n");
			}
			break;
		}

			   // ════════════════════════════════════════════════════════════
			   //  Utility
			   // ════════════════════════════════════════════════════════════

				  // ── 22. Rescan disc ────────────────────────────────────────
		case 22: {
			Console::Info("\nScanning drives...\n");
			wchar_t newAudioDrive = 0;
			std::vector<wchar_t> newAudioDrives;
			std::vector<wchar_t> newCdDrives = ScanDrives(newAudioDrives);

			if (newCdDrives.empty()) {
				Console::Error("No CD/DVD drives found!\n");
				break;
			}

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

			if (newAudioDrive != audioDrive) {
				copier.Close();
				if (!copier.Open(newAudioDrive)) {
					Console::Error("Failed to open drive\n");
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

			Console::Info("Rescanning disc...\n");
			disc = DiscInfo{};
			hasTOC = copier.ReadTOC(disc);
			bool didTOCScan = false;

			if (!hasTOC) {
				Console::Warning("No TOC found.\n");
				Console::Info("Attempting TOC-less disc scan from LBA 0...\n");
				hasTOC = copier.ScanDiscWithoutTOC(disc);
				didTOCScan = hasTOC;
				if (!hasTOC) {
					Console::Warning("Disc scan failed. Disc-dependent features will be unavailable.\n");
					break;
				}
			}

			// Bad TOC — rescan ONLY if ReadTOC was the source
			if (hasTOC && disc.tocRepaired && !didTOCScan) {
				Console::Warning("TOC had out-of-range entries that were clamped.\n");
				Console::Info("Re-scanning disc without TOC for accurate boundaries...\n");
				DiscInfo rescanned;
				if (copier.ScanDiscWithoutTOC(rescanned)) {
					disc = rescanned;
				}
				else {
					Console::Warning("TOC-less scan failed — using clamped TOC instead.\n");
				}
			}

			if (hasTOC) {
				copier.ReadCDText(disc);
				copier.ReadISRC(disc);
				std::vector<std::vector<uint32_t>> pressingCRCs;
				AccurateRip::Lookup(disc, pressingCRCs);
				PrintDiscInfo(disc);
				Console::Success("Disc rescan complete.\n");
			}
			break;
		}

			   // ── 23. Help ───────────────────────────────────────────────
		case 23:
			PrintHelpMenu();
			break;

			// ── 24. Exit ───────────────────────────────────────────────
		case 24:
			copier.Close();
			Console::Success("\nGoodbye!\n");
			return 0;

		default:
			Console::Warning("Unknown option.\n");
			break;
		}

		if (choice != 24) {
			WaitForKey();
		}
	}

	return 0;
}