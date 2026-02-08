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
	// Enable Virtual Terminal Processing for ANSI escape codes
	HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
	DWORD mode = 0;
	GetConsoleMode(hOut, &mode);
	SetConsoleMode(hOut, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
	SetConsoleOutputCP(CP_UTF8);

	// Apply custom look
	Console::SetFont(L"Cascadia Mono", 20);
	Console::SetWindowSize(110, 38);
	Console::ApplyDarkTheme();

	g_interrupt.Install();
	CenterConsoleWindow();
	std::wstring dir = GetWorkingDirectory();
	SetCurrentDirectoryW(dir.c_str());

	Console::BoxHeading("Audio CD Copy Tool");
	std::cout << Console::Sym::Vertical << " Working directory: ";
	std::wcout << dir << L"\n";
	InterruptHandler::PrintExitHelp();
	Console::BoxFooter();

	Console::Info("Scanning drives...\n");
	wchar_t audioDrive = 0;
	std::vector<wchar_t> audioDrives;
	std::vector<wchar_t> cdDrives = ScanDrives(audioDrives);

	if (cdDrives.empty()) {
		Console::Error("No CD/DVD drives found!\n");
		return 1;
	}

	if (audioDrives.size() == 1) {
		audioDrive = audioDrives[0];
	}
	else if (audioDrives.size() > 1) {
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
		audioDrive = audioDrives[pick - 1];
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
		Console::BoxHeading("Operation");
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
		PrintMenuItem(17, "Rescan disc");
		PrintMenuItem(18, "Exit", true);
		Console::BoxFooter();
		std::cout << Console::Sym::Arrow << " Choice: ";

		int choice = GetMenuChoice(1, 18, 1);
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
			Console::Info("This test reads sectors at different speeds to verify C2 accuracy.\n");
			Console::Info("Inconsistent C2 results may indicate unreliable C2 reporting.\n\n");

			// Test 3 spread-out LBAs instead of one
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

			int passed = 0;
			for (DWORD lba : testLBAs) {
				Console::Info("Testing LBA: ");
				std::cout << lba << "\n";
				if (copier.ValidateC2Accuracy(lba))
					passed++;
			}

			prog.Finish(true);

			if (passed == static_cast<int>(testLBAs.size())) {
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
			break;
		}

		case 17: {
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
				Console::Warning("\nMultiple audio CDs detected. Select drive:\n");
				for (size_t i = 0; i < newAudioDrives.size(); i++) {
					HANDLE h = OpenDriveHandle(newAudioDrives[i]);
					std::string name = (h != INVALID_HANDLE_VALUE) ? GetDriveName(h) : "CD/DVD drive";
					int tracks = (h != INVALID_HANDLE_VALUE) ? GetAudioTrackCount(h) : 0;
					if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
					std::cout << "  " << (i + 1) << ". [";
					Console::SetColor(Console::Color::Yellow);
					std::cout << static_cast<char>(newAudioDrives[i]) << ":";
					Console::Reset();
					std::cout << "] " << name;
					if (tracks > 0) std::cout << " (" << tracks << " tracks)";
					std::cout << "\n";
				}
				std::cout << "Choice: ";
				int pick = GetMenuChoice(1, static_cast<int>(newAudioDrives.size()), 1);
				std::cin.clear();
				if (std::cin.peek() == '\n') std::cin.ignore();
				newAudioDrive = newAudioDrives[pick - 1];
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

			Console::Info("Rescanning disc...\n");
			disc = DiscInfo{};
			if (!copier.ReadTOC(disc)) {
				Console::Error("Failed to read TOC. Is a disc inserted?\n");
				break;
			}
			copier.ReadCDText(disc);
			copier.ReadISRC(disc);
			AccurateRip::Lookup(disc);
			PrintDiscInfo(disc);
			Console::Success("Disc rescan complete.\n");
			break;
		}

		case 18:
			Console::Success("\nGoodbye!\n");
			return 0;
		}

		if (choice != 18) {
			WaitForKey();
		}
	}
}