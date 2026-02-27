// ============================================================================
// main.cpp - Application entry point and interactive menu loop
//
// Initialises the console, discovers CD/DVD drives, reads disc metadata
// (TOC, CD-TEXT, ISRC, AccurateRip), and presents a 21-item menu for
// copying, writing, scanning, and analysing audio CDs.
// ============================================================================
#define NOMINMAX  // Prevent Windows.h min/max macros from colliding with <algorithm>

#include "AudioCDCopier.h"
#include "AccurateRip.h"
#include "InterruptHandler.h"
#include "MenuHelpers.h"
#include "ConsoleColors.h"
#include "Drive.h"
#include "DriveSelection.h"
#include "FileUtils.h"
#include "MainMenu.h"
#include "MenuUI.h"
#include <windows.h>
#include <iostream>

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
	InterruptHandler::Instance().Install();

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

	// Show drive-specific recommendations
	DriveCapabilities caps;
	if (copier.DetectDriveCapabilities(caps)) {
		std::string recommendation = copier.GetDriveRecommendation();
		if (!recommendation.empty()) {
			Console::SetColor(Console::Color::Yellow);
			std::cout << "  " << recommendation << "\n";
			Console::Reset();
			std::cout << "  (View full recommendations: Option 14 - Drive capabilities)\n";
		}
	}
	std::cout << "\n";

	DiscInfo disc;
	bool hasTOC = copier.ReadTOC(disc);
	bool didTOCScan = false;

	if (!hasTOC) {
		Console::Warning("No TOC found (empty or blank disc).\n");
		Console::Info("Attempting TOC-less disc scan from LBA 0...\n");
		hasTOC = copier.ScanDiscWithoutTOC(disc);
		didTOCScan = hasTOC;
		if (!hasTOC) {
			Console::Warning("Disc scan failed. Disc-dependent features will be unavailable.\n");
		}
	}

	if (hasTOC) {
		// Bad TOC detected — rescan ONLY if ReadTOC was the source.
		// If ScanDiscWithoutTOC already ran, don't scan again.
		if (disc.tocRepaired && !didTOCScan) {
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
		copier.ReadCDText(disc);
		copier.ReadISRC(disc);
		std::vector<std::vector<uint32_t>> pressingCRCs;
		AccurateRip::Lookup(disc, pressingCRCs);
		PrintDiscInfo(disc);
	}

	return RunMainMenuLoop(copier, disc, dir, audioDrive, hasTOC);
}