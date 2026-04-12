// ============================================================================
// main.cpp - Application entry point and interactive menu loop
//
// Initialises the console, discovers CD/DVD drives, reads disc metadata
// (TOC, CD-TEXT, ISRC, AccurateRip), and presents a 27-item menu for
// copying, writing, scanning, and analysing audio CDs.
// ============================================================================

// Prevent Windows.h from defining min/max macros, which would collide with
// std::min / std::max from <algorithm> used elsewhere in the project.
#define NOMINMAX

#include "AudioCDCopier.h"      // High-level disc copy/scan orchestration class
#include "AccurateRip.h"        // CRC calculation and AccurateRip database lookup
#include "InterruptHandler.h"   // Ctrl+C / ESC signal handler (graceful cancellation)
#include "MenuHelpers.h"        // GetMenuChoice(), validated numeric input helper
#include "ConsoleColors.h"      // Console::SetColor(), Console::Info/Error/Warning helpers
#include "Drive.h"              // Low-level drive enumeration (ScanDrives)
#include "DriveSelection.h"     // SelectAudioDrive(), WaitForDisc() UI helpers
#include "FileUtils.h"          // GetWorkingDirectory(), file I/O utilities
#include "MainMenu.h"           // RunMainMenuLoop() — the 27-item interactive menu
#include "MenuUI.h"             // PrintMenuItem(), PrintMenuSection(), box-drawing helpers
#include "ExtractBackground.h"  // Windows Terminal profile creation and background theming
#include <windows.h>            // Win32 console API (handles, codepage, VT processing)
#include <iostream>             // std::cout / std::wcout for console output

int main() {
	// ── Console initialisation ──────────────────────────────────────────────
	// Obtain a handle to stdout so we can enable ANSI/VT100 escape-sequence
	// processing.  This allows Console::SetColor() to use "\033[..." codes
	// for coloured output instead of the legacy SetConsoleTextAttribute API.
	HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
	if (hOut != INVALID_HANDLE_VALUE && hOut != NULL) {
		DWORD mode = 0;
		if (GetConsoleMode(hOut, &mode)) {
			// ENABLE_VIRTUAL_TERMINAL_PROCESSING makes the console interpret
			// VT100 sequences (colours, cursor movement, box-drawing chars).
			SetConsoleMode(hOut, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
		}
	}
	// Switch the console output codepage to UTF-8 so Unicode characters in
	// CD-TEXT titles, ISRC codes, and box-drawing symbols render correctly.
	SetConsoleOutputCP(CP_UTF8);

	// ── Background & profile (before visual setup to minimise flash) ────────
	// Extract the embedded PNG background image from the .exe's Win32
	// resources to %LOCALAPPDATA%\AudioCopy\background.png.  Then inject
	// (or update) a dedicated "AudioCopy" profile into Windows Terminal's
	// settings.json with that image, a dark background colour, and opacity.
	// If we're not already running inside that profile, relaunch the app
	// in a new WT tab that uses it, and exit this instance immediately.
	std::wstring bgPath = ExtractBackgroundImage();
	if (!bgPath.empty()) {
		EnsureAudioCopyProfile(bgPath);
		if (RelaunchInAudioCopyProfile()) {
			return 0; // New WT window has the profile — exit this instance
		}
	}

	// ── Visual console setup (only reached inside the AudioCopy profile) ────
	// Configure the console's appearance now that we know we're in the
	// branded WT profile (or a legacy conhost fallback).
	Console::SetFont(L"Cascadia Mono", 20);  // Monospaced font for aligned tables/graphs
	Console::SetWindowSize(110, 38);          // Wide enough for progress bars and reports
	Console::ApplyDarkTheme();                // Dark background + light text colour scheme

	// Install a signal handler that catches Ctrl+C (SIGINT) and polls for
	// ESC key presses.  Long-running scans check g_interrupt periodically
	// so they can abort cleanly without leaving the drive in a bad state.
	InterruptHandler::Instance().Install();

	// Centre the console window on the primary monitor and resolve the
	// working directory where output files (.bin, .cue, .wav, logs) are saved.
	CenterConsoleWindow();
	std::wstring dir = GetWorkingDirectory();
	SetCurrentDirectoryW(dir.c_str());

	// Print the branded header box with the working directory path and
	// a reminder of how to exit (ESC / Ctrl+C).
	Console::BoxHeading("Audio CD Copy Tool");
	std::cout << Console::Sym::Vertical << " Working directory: ";
	std::wcout << dir << L"\n";
	InterruptHandler::PrintExitHelp();
	Console::BoxFooter();

	// ── Drive discovery ─────────────────────────────────────────────────────
	// Enumerate all CD/DVD optical drives on the system by scanning drive
	// letters A–Z for devices with DRIVE_CDROM type.  Drives that already
	// contain an audio CD are separated into `audioDrives` so we can
	// auto-select when only one disc is present.
	Console::Info("Scanning drives...\n");
	wchar_t audioDrive = 0;
	std::vector<wchar_t> audioDrives;
	std::vector<wchar_t> cdDrives = ScanDrives(audioDrives);

	// If the system has no optical drives at all, there's nothing to do.
	if (cdDrives.empty()) {
		Console::Error("No CD/DVD drives found!\n");
		return 1;
	}

	// Auto-select if exactly one audio disc is present.  If multiple audio
	// discs are found, display a numbered list and let the user choose.
	if (audioDrives.size() == 1) {
		audioDrive = audioDrives[0];
	}
	else if (audioDrives.size() > 1) {
		audioDrive = SelectAudioDrive(audioDrives);
	}

	// No audio disc found at startup — poll all drives every few seconds
	// until the user inserts one or presses ESC to give up.
	if (!audioDrive) {
		audioDrive = WaitForDisc(cdDrives, 0);
		if (!audioDrive) {
			// The user didn't insert a disc.  Rather than exiting, let them
			// pick any available drive so drive-level features (capabilities
			// query, write-speed enumeration, chipset ID) still work.
			if (cdDrives.size() == 1) {
				// Only one drive exists — use it automatically.
				audioDrive = cdDrives[0];
				Console::Warning("No audio disc detected. Using drive ");
				Console::SetColor(Console::Color::Yellow);
				std::cout << static_cast<char>(audioDrive) << ":";
				Console::Reset();
				std::cout << " for drive-level operations.\n";
			}
			else if (cdDrives.size() > 1) {
				// Multiple drives — present a selection menu.
				Console::Warning("No audio disc detected. Select a drive for drive-level operations:\n");
				for (size_t i = 0; i < cdDrives.size(); i++) {
					std::cout << "  " << (i + 1) << ". [";
					Console::SetColor(Console::Color::Yellow);
					std::cout << static_cast<char>(cdDrives[i]) << ":";
					Console::Reset();
					std::cout << "]\n";
				}
				std::cout << "Choice: ";
				// GetMenuChoice() reads a validated integer in [min, max],
				// returning the default (1) if the user just presses Enter.
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

	// Confirm the selected drive letter to the user.
	std::cout << "\nUsing drive ";
	Console::SetColor(Console::Color::Yellow);
	std::cout << static_cast<char>(audioDrive) << ":";
	Console::Reset();
	std::cout << "\n";

	// ── Open drive and read disc metadata ───────────────────────────────────
	// Open a raw SCSI pass-through handle (\\.\X:) to the selected drive.
	// This bypasses the Windows CD-ROM class driver and gives us direct
	// access to MMC commands (READ CD, READ TOC, SET CD SPEED, etc.).
	AudioCDCopier copier;
	if (!copier.Open(audioDrive)) {
		Console::Error("Failed to open drive\n");
		return 1;
	}

	// Probe the drive's hardware capabilities (C2 error reporting, Accurate
	// Stream, overread, subchannel modes) and display any drive-specific
	// recommendations (e.g. "Plextor detected — Q-Check available").
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

	// ── Read Table of Contents ──────────────────────────────────────────────
	// The TOC is the primary source of track boundaries.  It lists each
	// track's start LBA, audio/data flag, session number, and control byte.
	DiscInfo disc;
	bool hasTOC = copier.ReadTOC(disc);
	bool didTOCScan = false;

	// If the TOC is missing or unreadable (blank disc, severely damaged
	// lead-in), fall back to a brute-force subchannel scan starting at
	// LBA 0.  This reads Q subchannel data sector-by-sector to reconstruct
	// track boundaries from the track/index fields encoded on disc.
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
		// Some drives return corrupt TOC LBAs (e.g. 0xFFFFFFE2 from a
		// firmware sign-extension bug).  ReadTOC clamps those values, but
		// the clamped boundaries may be inaccurate.  If Full TOC data
		// couldn't recover the real LBAs either, re-scan from scratch
		// using the TOC-less subchannel method for accurate boundaries.
		if (disc.tocRepaired && !disc.tocLBAsRecovered && !didTOCScan) {
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

		// Read optional metadata that enriches the rip output:
		// • CD-TEXT: album title, artist, and per-track titles/performers
		//   encoded in the lead-in subchannel (requires drive support).
		copier.ReadCDText(disc);

		// • ISRC: International Standard Recording Code — a 12-character
		//   per-track identifier used by the music industry for royalty
		//   tracking.  Stored in Mode-3 Q subchannel frames.
		copier.ReadISRC(disc);

		// • AccurateRip: query the online database using disc IDs derived
		//   from the TOC geometry.  Returns per-track CRCs from multiple
		//   verified pressings so the ripped audio can be validated later.
		std::vector<std::vector<uint32_t>> pressingCRCs;
		AccurateRip::Lookup(disc, pressingCRCs);

		// Display a formatted summary of the disc: track count, total
		// duration, CD-TEXT metadata, and AccurateRip match status.
		PrintDiscInfo(disc);
	}

	// ── Enter main menu ─────────────────────────────────────────────────────
	// Hand off to the 27-item interactive menu loop (MainMenu.cpp).  This
	// runs until the user selects "Exit" (option 27) or the process is
	// interrupted.  The menu provides ripping, quality scanning, disc info,
	// drive diagnostics, and utility operations.
	return RunMainMenuLoop(copier, disc, dir, audioDrive, hasTOC);
}