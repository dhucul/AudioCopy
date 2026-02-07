#include "MenuUI.h"
#include "ConsoleColors.h"
#include <windows.h>
#include <iostream>
#include <conio.h>

void CenterConsoleWindow() {
	Sleep(100);

	HWND consoleWnd = GetConsoleWindow();
	if (!consoleWnd) return;

	HWND targetWnd = consoleWnd;
	HWND parent = GetAncestor(consoleWnd, GA_ROOTOWNER);
	if (parent && parent != consoleWnd) {
		targetWnd = parent;
	}

	RECT workArea;
	if (!SystemParametersInfoW(SPI_GETWORKAREA, 0, &workArea, 0)) return;

	int screenWidth = workArea.right - workArea.left;
	int screenHeight = workArea.bottom - workArea.top;

	RECT windowRect;
	if (!GetWindowRect(targetWnd, &windowRect)) return;

	int windowWidth = windowRect.right - windowRect.left;
	int windowHeight = windowRect.bottom - windowRect.top;

	int posX = workArea.left + (screenWidth - windowWidth) / 2;
	int posY = workArea.top + (screenHeight - windowHeight) / 2;

	SetWindowPos(targetWnd, nullptr, posX, posY, 0, 0,
		SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
}

void WaitForKey(const char* message) {
	Console::Info(message);
	_getch();
}

void PrintMenuItem(int num, const char* text, bool dimmed) {
	Console::SetColor(dimmed ? Console::Color::DarkGray : Console::Color::Cyan);
	std::cout << (num < 10 ? " " : "") << num << ".";
	Console::Reset();
	std::cout << " " << text << "\n";
}

void PrintHelpMenu() {
	Console::Heading("\n=== Help - Test Descriptions ===\n");

	struct HelpItem { const char* title; const char* desc; const char* best; };
	const HelpItem items[] = {
		{"1. Copy Disc",
		 "Rips audio tracks to WAV/FLAC files with optional AccurateRip verification.\n"
		 "   Supports drive offset correction, subchannel extraction, pre-gap extraction,\n"
		 "   secure rip modes (burst/standard/paranoid), and detailed logging.",
		 "Creating high-quality digital backups of your audio CDs."},
		{"2. C2 Error Scan (Quick)",
		 "Performs a disc quality scan using the drive's C2 error reporting capability.\n"
		 "   Auto-detects best C2 mode: error pointers (accurate), error block (standard),\n"
		 "   or Plextor vendor commands for optimal drive-specific performance.\n"
		 "\n"
		 "   Sensitivity modes:\n"
		 "   - Standard (single-pass): Fast screening, bit-level counting\n"
		 "   - PlexTools-style: Multi-pass with cache defeat for accuracy\n"
		 "   - PlexTools-style (fast): Multi-pass without cache defeat\n"
		 "   - Paranoid: Maximum passes with cache defeat (slowest, most thorough)\n"
		 "\n"
		 "   C2 errors indicate uncorrectable read errors. Use Standard for quick health\n"
		 "   checks before ripping, or Paranoid mode for detailed analysis of damaged discs.",
		 "Quick disc health assessment or detailed error analysis before ripping."},
		{"3. BLER Scan (Detailed)",
		 "Measures Block Error Rate - the frequency of raw errors before correction.\n"
		 "   Provides detailed error distribution graphs across the disc surface.\n"
		 "   Red Book standard: BLER should be < 220 errors/second average.",
		 "Professional-grade disc quality analysis."},
		{"4. Disc Rot Detection",
		 "Analyzes error patterns to detect physical degradation (disc rot/bronzing).\n"
		 "   Checks for characteristic edge deterioration and oxidation patterns.",
		 "Evaluating older discs or checking storage conditions."},
		{"5. Speed Comparison Test",
		 "Tests read performance at multiple speeds to find optimal ripping speed.\n"
		 "   Slower speeds often yield better results on damaged discs.",
		 "Determining the best speed for problematic discs."},
		{"6. Lead Area Check",
		 "Examines lead-in and lead-out areas for hidden data or damage.\n"
		 "   These areas contain TOC data and are critical for disc recognition.",
		 "Diagnosing discs that fail to load or have TOC issues."},
		{"7. Generate Surface Map",
		 "Creates a visual representation of the entire disc surface quality.\n"
		 "   Shows error density patterns, scratch locations, and problem areas.",
		 "Visual documentation of disc condition."},
		{"8. Multi-Pass Verification",
		 "Reads the disc multiple times and compares results for consistency.\n"
		 "   Inconsistent reads indicate marginal sectors or drive issues.",
		 "Maximum confidence in rip accuracy."},
		{"9. Audio Content Analysis",
		 "Analyzes audio characteristics: silence detection, clipping, levels.\n"
		 "   Detects pre-emphasis, HDCD encoding, and dynamic range.",
		 "Understanding the audio mastering of the disc."},
		{"10. Drive Capabilities",
		 "Detects and displays your CD/DVD drive's hardware capabilities.\n"
		 "   Shows support for: C2 errors, accurate stream, CD-TEXT, subchannel.\n"
		 "   Also displays: read/write speeds, buffer size, overread capability.\n"
		 "   Provides a ripping quality score to assess drive suitability.",
		 "Checking if your drive is suitable for accurate ripping."},
		{"11. Disc Fingerprint",
		 "Generates unique disc identifiers for online database lookups:\n"
		 "   - CDDB/FreeDB ID for metadata lookup\n"
		 "   - MusicBrainz Disc ID for accurate metadata matching\n"
		 "   - AccurateRip IDs for rip verification\n"
		 "   - Audio content hash for duplicate detection",
		 "Looking up album metadata or verifying disc identity."},
		{"12. Subchannel Integrity",
		 "Verifies the integrity of subchannel data (Q-channel timing, etc.).\n"
		 "   Subchannel errors can cause incorrect track indexing or timing issues.",
		 "Diagnosing timing/indexing issues or verifying subchannel extraction."},
		{"13. Seek Time Analysis",
		 "Measures drive seek performance across the disc surface.\n"
		 "   Slow seeks may indicate mechanical issues or disc damage.",
		 "Diagnosing drive performance or disc readability issues."},
		{"14. Drive Offset Detection",
		 "Automatically detects your CD drive's read offset using AccurateRip database.\n"
		 "   Offset correction ensures sample-accurate rips that match the original master.",
		 "Configuring your drive for accurate ripping."},
		{"15. Help (test descriptions)",
		 "Displays detailed descriptions for each test available in the tool.",
		 "Understanding the purpose and details of each operation."},
		{"16. C2 Validation Test",
		 "Tests the reliability of your drive's C2 error reporting at different speeds.\n"
		 "   Some drives report false C2 errors at high speeds. This test verifies accuracy\n"
		 "   by comparing C2 results at slow and fast speeds for consistency.",
		 "Determining if your drive's C2 detection is trustworthy before scanning."},
		{"17. Exit",
		 "Exits the program.",
		 "Closing the tool when done."},
	};

	for (const auto& item : items) {
		Console::SetColor(Console::Color::Cyan);
		std::cout << item.title << "\n";
		Console::Reset();
		std::cout << "   " << item.desc << "\n";
		std::cout << "   Best for: " << item.best << "\n\n";
	}

	Console::SetColor(Console::Color::DarkGray);
	std::cout << "Note: Administrator privileges are recommended for SCSI pass-through commands.\n";
	Console::Reset();
}