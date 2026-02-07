#define NOMINMAX

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

int main() {
	g_interrupt.Install();
	CenterConsoleWindow();
	std::wstring dir = GetWorkingDirectory();
	SetCurrentDirectoryW(dir.c_str());

	Console::Heading("=== Audio CD Copy Tool ===\n");
	std::cout << "Working directory: ";
	std::wcout << dir << L"\n";
	InterruptHandler::PrintExitHelp();

	Console::Info("Scanning drives...\n");
	wchar_t audioDrive = 0;
	std::vector<wchar_t> cdDrives = ScanDrives(audioDrive);

	if (cdDrives.empty()) {
		Console::Error("No CD/DVD drives found!\n");
		return 1;
	}

	if (!audioDrive) {
		audioDrive = WaitForDisc(cdDrives, 0);
		if (!audioDrive) {
			Console::Error("No disc selected.\n");
			return 1;
		}
	}

	std::cout << "\nUsing drive ";
	Console::SetColor(Console::Color::Yellow);
	std::cout << static_cast<char>(audioDrive) << ":";
	Console::Reset();
	std::cout << "\n";

	AudioCDCopier copier;
	if (!copier.Open(audioDrive)) {
		Console::Error("Failed to open drive\n");
		return 1;
	}

	DiscInfo disc;
	if (!copier.ReadTOC(disc)) {
		Console::Error("Failed to read TOC\n");
		return 1;
	}

	copier.ReadCDText(disc);
	copier.ReadISRC(disc);
	AccurateRip::Lookup(disc);
	PrintDiscInfo(disc);

	// Main menu loop
	while (true) {
		Console::Heading("\n=== Operation ===\n");
		PrintMenuItem(1, "Copy disc");
		PrintMenuItem(2, "C2 error scan (quick)");
		PrintMenuItem(3, "BLER scan (detailed)");
		PrintMenuItem(4, "Disc rot detection");
		PrintMenuItem(5, "Speed comparison test");
		PrintMenuItem(6, "Lead area check");
		PrintMenuItem(7, "Generate surface map");
		PrintMenuItem(8, "Multi-pass verification");
		PrintMenuItem(9, "Audio content analysis");
		PrintMenuItem(10, "Drive capabilities");
		PrintMenuItem(11, "Disc fingerprint (CDDB/MusicBrainz/AccurateRip IDs)");
		PrintMenuItem(12, "Subchannel integrity check");
		PrintMenuItem(13, "Seek time analysis");
		PrintMenuItem(14, "Drive offset detection");
		PrintMenuItem(15, "Help (test descriptions)");
		PrintMenuItem(16, "C2 validation test");
		PrintMenuItem(17, "Exit", true);
		std::cout << "Choice: ";

		int choice = GetMenuChoice(1, 17, 1);
		std::cin.clear();
		if (std::cin.peek() == '\n') {
			std::cin.ignore();
		}

		switch (choice) {
		case 1:
			RunCopyWorkflow(copier, disc, dir);
			break;

		case 2: {
			int speed = copier.SelectScanSpeed();
			if (speed == -1) break;
			int sensitivity = copier.SelectC2Sensitivity();
			if (sensitivity == -1) break;
			copier.ScanDiscForC2Errors(disc, speed, sensitivity);
			break;
		}

		case 3: {
			int speed = copier.SelectScanSpeed();
			if (speed == -1) break;
			BlerResult result;
			if (copier.RunBlerScan(disc, result, speed)) {
				copier.PrintBlerGraph(result);
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

		case 4: {
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

		case 5: {
			std::vector<SpeedComparisonResult> results;
			copier.RunSpeedComparisonTest(disc, results);
			break;
		}

		case 6: {
			int speed = copier.SelectScanSpeed();
			if (speed == -1) break;
			copier.CheckLeadAreas(disc, speed);
			break;
		}

		case 7: {
			int speed = copier.SelectScanSpeed();
			if (speed == -1) break;
			std::wstring mapFile = dir + L"\\surface_map.txt";
			copier.GenerateSurfaceMap(disc, mapFile, speed);
			Console::Success("Surface map saved to: ");
			std::wcout << mapFile << L"\n";
			break;
		}

		case 8: {
			int speed = copier.SelectScanSpeed();
			if (speed == -1) break;
			std::cout << "\n=== Multi-Pass Verification ===\n";
			std::cout << "Select number of passes (2-10, recommended: 3): ";
			int passes = GetMenuChoice(2, 10, 3);
			std::vector<MultiPassResult> results;
			copier.RunMultiPassVerification(disc, results, passes, speed);
			break;
		}

		case 9: {
			int speed = copier.SelectScanSpeed();
			if (speed == -1) break;
			AudioAnalysisResult result;
			copier.AnalyzeAudioContent(disc, result, speed);
			break;
		}

		case 10: {
			DriveCapabilities caps;
			if (copier.DetectDriveCapabilities(caps)) {
				copier.PrintDriveCapabilities(caps);
			}
			else {
				Console::Error("Failed to detect drive capabilities.\n");
			}
			break;
		}

		case 11: {
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

		case 12: {
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

		case 13: {
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

		case 14: {
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

		case 15:
			PrintHelpMenu();
			break;

		case 16: {
			Console::Info("\n=== C2 Validation Test ===\n");
			Console::Info("This test reads a sector at different speeds to verify C2 accuracy.\n");
			Console::Info("Inconsistent C2 results may indicate unreliable C2 reporting.\n\n");

			DWORD trackLength = disc.tracks[0].endLBA - disc.tracks[0].startLBA;
			DWORD testLBA = disc.tracks[0].startLBA + 1000;
			if (testLBA >= disc.tracks[0].endLBA) {
				testLBA = disc.tracks[0].startLBA + (trackLength / 2);
			}

			Console::Info("Testing LBA: ");
			std::cout << testLBA << " (Track 1, middle section)\n";

			ProgressIndicator prog;
			prog.SetLabel("Validating C2");
			prog.Start();

			bool isReliable = copier.ValidateC2Accuracy(testLBA);

			prog.Finish(true);

			if (isReliable) {
				Console::Success("\nC2 Validation: PASSED\n");
				Console::Success("Your drive's C2 error reporting appears reliable.\n");
				Console::Info("C2 results were consistent across different read speeds.\n");
			}
			else {
				Console::Warning("\nC2 Validation: FAILED\n");
				Console::Warning("Your drive's C2 error reporting may be unreliable.\n");
				Console::Warning("C2 results differed at different speeds - this drive may report\n");
				Console::Warning("false positives. Consider using BLER scan instead for quality checks.\n");
			}
			break;
		}

		case 17:
			Console::Success("\nGoodbye!\n");
			return 0;
		}

		if (choice != 17) {
			WaitForKey();
		}
	}
}