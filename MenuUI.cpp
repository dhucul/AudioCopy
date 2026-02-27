// ============================================================================
// MenuUI.cpp - Console UI helper functions
//
// Provides window centering, key-wait, menu item rendering, and a detailed
// help screen describing every operation available in the tool.
// ============================================================================
#include "MenuUI.h"
#include "ConsoleColors.h"
#include <windows.h>
#include <iostream>
#include <conio.h>

// ── CenterConsoleWindow ─────────────────────────────────────────────────────
// Positions the console (or its top-level owner in Windows Terminal) at the
// exact centre of the primary monitor's work area (excludes the taskbar).
void CenterConsoleWindow() {
	Sleep(100);  // Brief delay to let the window finish initialising after resize

	HWND consoleWnd = GetConsoleWindow();
	if (!consoleWnd) return;

	// In Windows Terminal the console HWND is hosted inside a parent window;
	// resolve the root owner so we move the correct frame.
	HWND targetWnd = consoleWnd;
	HWND parent = GetAncestor(consoleWnd, GA_ROOTOWNER);
	if (parent && parent != consoleWnd) {
		targetWnd = parent;
	}

	// Retrieve the usable desktop area (screen minus the taskbar).
	RECT workArea;
	if (!SystemParametersInfoW(SPI_GETWORKAREA, 0, &workArea, 0)) return;

	int screenWidth = workArea.right - workArea.left;
	int screenHeight = workArea.bottom - workArea.top;

	// Measure the window's current dimensions.
	RECT windowRect;
	if (!GetWindowRect(targetWnd, &windowRect)) return;

	int windowWidth = windowRect.right - windowRect.left;
	int windowHeight = windowRect.bottom - windowRect.top;

	// Calculate centred coordinates.
	int posX = workArea.left + (screenWidth - windowWidth) / 2;
	int posY = workArea.top + (screenHeight - windowHeight) / 2;

	// Reposition without resizing (SWP_NOSIZE) or changing Z-order (SWP_NOZORDER).
	SetWindowPos(targetWnd, nullptr, posX, posY, 0, 0,
		SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
}

// ── WaitForKey ──────────────────────────────────────────────────────────────
// Displays an info message and blocks until any key is pressed.  Uses _getch()
// so the keystroke is not echoed and Enter is not required.
void WaitForKey(const char* message) {
	Console::Info(message);
	_getch();
}

// ── PrintMenuItem ───────────────────────────────────────────────────────────
// Renders a single menu line like "  3. Some option" with coloured numbering.
// When `dimmed` is true the number is dark grey instead of cyan (used for the
// Exit item to visually de-emphasise it).
void PrintMenuItem(int num, const char* text, bool dimmed) {
	Console::SetColor(dimmed ? Console::Color::DarkGray : Console::Color::Cyan);
	std::cout << (num < 10 ? " " : "") << num << ".";  // Right-align single digits
	Console::Reset();
	std::cout << " " << text << "\n";
}

// ── PrintMenuSection ────────────────────────────────────────────────────────
// Renders a dimmed category header inside the menu box to visually group
// related operations (e.g. "── Disc Quality ──").
void PrintMenuSection(const char* label) {
	Console::SetColor(Console::Color::DarkGray);
	std::cout << "  -- " << label << " --\n";
	Console::Reset();
}

// ── PrintHelpMenu ───────────────────────────────────────────────────────
// Outputs a detailed help screen listing every available operation with a
// multi-line description and a "Best for:" hint.  Entries are grouped by
// category with visible section headers matching the main menu layout.
void PrintHelpMenu() {
	Console::Heading("\n=== Help - Test Descriptions ===\n");

	// Each HelpItem bundles a title, detailed description, and best-use hint.
	struct HelpItem { const char* title; const char* desc; const char* best; };

	// Helper lambda to print a single help entry with coloured title.
	auto PrintEntry = [](const HelpItem& item) {
		Console::SetColor(Console::Color::Cyan);
		std::cout << item.title << "\n";
		Console::Reset();
		std::cout << "   " << item.desc << "\n";
		std::cout << "   Best for: " << item.best << "\n\n";
		};

	// Helper lambda to print a section header matching the main menu style.
	auto PrintSection = [](const char* label) {
		Console::SetColor(Console::Color::DarkGray);
		std::cout << "-- " << label << " --\n\n";
		Console::Reset();
		};

	// ═════════════════════════════════════════════════════════════════════
	//  Ripping
	// ═════════════════════════════════════════════════════════════════════
	PrintSection("Ripping");

	PrintEntry({ "1. Copy Disc",
		"Rips audio tracks to WAV/FLAC files with optional AccurateRip verification.\n"
		"   Supports drive offset correction, subchannel extraction, pre-gap extraction,\n"
		"   secure rip modes (burst/standard/paranoid), and detailed logging.",
		"Creating high-quality digital backups of your audio CDs." });

	PrintEntry({ "2. Write Disc (.bin/.cue/.sub Files)",
		"Burns audio data back to a blank CD-R/CD-RW from .bin/.cue/.sub files.\n"
		"   Supports writing audio images previously ripped with this tool,\n"
		"   including subchannel data restoration when .sub files are present.",
		"Creating accurate copies of audio CDs from previously ripped images." });

	// ═════════════════════════════════════════════════════════════════════
	//  Disc Quality
	// ═════════════════════════════════════════════════════════════════════
	PrintSection("Disc Quality");

	PrintEntry({ "3. C2 Error Scan",
		"Performs a disc quality scan using the drive's C2 error reporting capability.\n"
		"   Auto-detects best C2 mode: error pointers (standard MMC), error block,\n"
		"   or Plextor vendor D8 commands when available.\n"
		"\n"
		"   Uses multi-pass scanning with cache defeat for accuracy.\n"
		"   Error sectors are re-read to verify results.\n"
		"\n"
		"   C2 errors indicate uncorrectable read errors. Use this for disc health\n"
		"   checks before ripping or detailed analysis of damaged discs.",
		"Quick disc health assessment or detailed error analysis before ripping." });

	PrintEntry({ "4. BLER Scan (Detailed)",
		"Measures Block Error Rate - the frequency of raw errors before correction.\n"
		"   Provides per-track statistics, error clustering analysis, zone distribution,\n"
		"   and a text-based error graph across the disc surface.\n"
		"\n"
		"   C1 block error reporting is auto-detected at startup.  Drives that populate\n"
		"   bytes 294-295 of the C2 response (Plextor D8, many LiteOn/ASUS/Pioneer\n"
		"   drives in ErrorPointers mode) will show full C1+C2 statistics.\n"
		"   Other drives report C2 errors only.\n"
		"\n"
		"   Red Book standard: average BLER should be < 220 errors/second.",
		"Professional-grade disc quality analysis with C1/C2 breakdown." });

	PrintEntry({ "5. Disc Rot Detection",
		"Analyzes error patterns to detect physical degradation (disc rot/bronzing).\n"
		"   Checks for characteristic edge deterioration and oxidation patterns.",
		"Evaluating older discs or checking storage conditions." });

	PrintEntry({ "6. Generate Surface Map",
		"Creates a visual representation of the entire disc surface quality.\n"
		"   Shows error density patterns, scratch locations, and problem areas.\n"
		"   Outputs a text file with a grid of symbols indicating sector health.",
		"Visual documentation of disc condition or identifying damaged regions." });

	PrintEntry({ "7. Multi-Pass Verification",
		"Reads the disc multiple times (2-10 passes) and compares results.\n"
		"   Inconsistent reads indicate marginal sectors or drive issues.\n"
		"   Sectors that differ between passes are flagged as unreliable.",
		"Maximum confidence in rip accuracy." });

	// ═════════════════════════════════════════════════════════════════════
	//  Disc Information
	// ═════════════════════════════════════════════════════════════════════
	PrintSection("Disc Info");

	PrintEntry({ "8. Audio Content Analysis",
		"Analyzes audio characteristics: silence detection, clipping, levels.\n"
		"   Detects pre-emphasis, HDCD encoding, and dynamic range.\n"
		"   Reports peak levels and average loudness per track.",
		"Understanding the audio mastering of the disc." });

	PrintEntry({ "9. Disc Fingerprint (CDDB/MusicBrainz/AccurateRip IDs)",
		"Generates unique disc identifiers for online database lookups:\n"
		"   - CDDB/FreeDB ID for metadata lookup\n"
		"   - MusicBrainz Disc ID for accurate metadata matching\n"
		"   - AccurateRip IDs for rip verification\n"
		"   - Audio content hash for duplicate detection\n"
		"   Results are saved to disc_fingerprint.txt.",
		"Looking up album metadata or verifying disc identity." });

	PrintEntry({ "10. Lead Area Check",
		"Examines lead-in and lead-out areas for hidden data or damage.\n"
		"   These areas contain TOC data and are critical for disc recognition.\n"
		"   Can reveal hidden track zero audio (HTOA) or pre-gap content.",
		"Diagnosing discs that fail to load or have TOC issues." });

	PrintEntry({ "11. Subchannel Integrity Check",
		"Verifies the integrity of subchannel data (Q-channel timing, etc.).\n"
		"   Subchannel errors can cause incorrect track indexing or timing issues.\n"
		"   Reports total error count across all sectors.",
		"Diagnosing timing/indexing issues or verifying subchannel extraction." });

	PrintEntry({ "12. Verify Subchannel Burn Status",
		"Samples sectors across the disc and reads raw subchannel data to determine\n"
		"   whether subchannel information was actually mastered/burned onto the disc.\n"
		"   Checks Q-channel CRC validity, P-channel pause/play state, R-W channel\n"
		"   content (CD-G graphics), and MSF timing consistency.\n"
		"\n"
		"   Pressed/mastered CDs always have valid subchannel data. Burned CD-Rs may\n"
		"   or may not, depending on the burning software and settings used.",
		"Deciding if subchannel extraction is useful before ripping, or identifying burned copies vs. originals." });

	PrintEntry({ "13. Copy-Protection Check",
		"Scans the disc for common audio CD copy-protection mechanisms.\n"
		"   Performs 8 heuristic checks:\n"
		"   - Illegal / non-standard TOC entries\n"
		"   - Multi-session abuse (> 2 sessions)\n"
		"   - Data tracks mixed with audio (XCP / MediaMax indicator)\n"
		"   - Pre-emphasis flag anomalies\n"
		"   - Non-standard track gap sizes\n"
		"   - Intentional read errors (CDS / Key2Audio / MediaClyS)\n"
		"   - Subchannel data manipulation (Q-channel CRC / MSF jumps)\n"
		"   - Lead-in overread blocking\n"
		"\n"
		"   Results are aggregated into an overall verdict with a best-guess\n"
		"   identification of the protection scheme. Report saved to\n"
		"   protection_check.txt.",
		"Determining whether a disc uses copy protection before ripping." });

	// ═════════════════════════════════════════════════════════════════════
	//  Drive Diagnostics
	// ═════════════════════════════════════════════════════════════════════
	PrintSection("Drive");

	PrintEntry({ "14. Drive Capabilities",
		"Detects and displays your CD/DVD drive's hardware capabilities.\n"
		"   Shows support for: C2 errors, accurate stream, CD-TEXT, subchannel.\n"
		"   Also displays: read/write speeds, buffer size, overread capability.\n"
		"   Provides a ripping quality score to assess drive suitability.",
		"Checking if your drive is suitable for accurate ripping." });

	PrintEntry({ "15. Drive Offset Detection",
		"Automatically detects your CD drive's read offset using AccurateRip database.\n"
		"   Offset correction ensures sample-accurate rips that match the original master.\n"
		"   Displays the detected offset in samples along with a confidence percentage.",
		"Configuring your drive for accurate ripping." });

	PrintEntry({ "16. C2 Validation Test",
		"Tests the reliability of your drive's C2 error reporting at different speeds.\n"
		"   Some drives report false C2 errors at high speeds. This test verifies accuracy\n"
		"   by comparing C2 results at slow and fast speeds for consistency.\n"
		"   Tests up to 3 sectors spread across inner, middle, and outer disc regions.",
		"Determining if your drive's C2 detection is trustworthy before scanning." });

	PrintEntry({ "17. Speed Comparison Test",
		"Tests read performance at multiple speeds to find optimal ripping speed.\n"
		"   Slower speeds often yield better results on damaged discs.\n"
		"   Compares C2 error counts at each speed to identify the best trade-off.",
		"Determining the best speed for problematic discs." });

	PrintEntry({ "18. Seek Time Analysis",
		"Measures drive seek performance across the disc surface.\n"
		"   Slow seeks may indicate mechanical issues or disc damage.\n"
		"   Tests seek latency at various positions from inner to outer edge.",
		"Diagnosing drive performance or disc readability issues." });

	PrintEntry({ "19. Quality Scan (C1/C2/CU Graphs)",
		"Performs a hardware-driven CIRC error measurement using Plextor vendor\n"
		"   commands (0xE9/0xEB). The drive enters a dedicated scan mode at ~1x,\n"
		"   reporting C1, C2, and CU (uncorrectable) error counts per time slice\n"
		"   without transferring audio data.\n"
		"\n"
		"   This is the same measurement QPXTool's Q-Check performs. It provides\n"
		"   true C1 error rates that the standard BLER scan cannot measure.\n"
		"\n"
		"   Requires a classic Plextor drive:\n"
		"     PX-708A, PX-712A/SA, PX-716A/SA/AL, PX-755A/SA, PX-760A/SA\n"
		"\n"
		"   Other Plextor/Lite-On drives (PX-891SAF, etc.) support D8 reads\n"
		"   but not Q-Check. Use option 4 (BLER Scan) on those drives instead.",
		"True C1/C2/CU error rate measurement on supported Plextor drives." });

	PrintEntry({ "20. Chipset Identification",
		"Identifies the internal chipset / controller used by your CD/DVD drive.\n"
		"   Displays chipset vendor, model, and firmware-level details when available.\n"
		"   Useful for determining hardware-level capabilities not exposed via standard\n"
		"   MMC feature queries.",
		"Identifying drive hardware for compatibility or capability research." });

	PrintEntry({ "21. Disc Balance Check",
		"Detects disc wobble or eccentricity by reading sample sectors at increasing\n"
		"   speeds (4x through 40x) and measuring how C2 errors change.\n"
		"\n"
		"   A well-balanced disc maintains low error rates across all speeds.\n"
		"   An unbalanced disc vibrates at high RPM, causing a sharp spike in\n"
		"   C2 errors and read failures above a certain speed threshold.\n"
		"\n"
		"   Reports a 0-100 balance score and recommends a safe rip speed.",
		"Diagnosing vibration/wobble issues or choosing optimal rip speed for warped/unbalanced discs." });

	// ═════════════════════════════════════════════════════════════════════
	//  Utility
	// ═════════════════════════════════════════════════════════════════════
	PrintSection("Utility");

	PrintEntry({ "22. Rescan Disc",
		"Re-scans drives and reloads disc metadata (TOC, CD-TEXT, ISRC, AccurateRip).\n"
		"   Automatically detects if the drive letter changed and re-opens the handle.\n"
		"   Supports switching between multiple drives if more than one is present.",
		"Use after swapping discs without restarting the program." });

	PrintEntry({ "23. Help (Test Descriptions)",
		"Displays this help screen with detailed descriptions of each operation.",
		"Understanding the purpose and details of each operation." });

	PrintEntry({ "24. Exit",
		"Exits the program.",
		"Closing the tool when done." });

	// Footer note about required privileges for raw SCSI access.
	Console::SetColor(Console::Color::DarkGray);
	std::cout << "Note: Administrator privileges are recommended for SCSI pass-through commands.\n";
	Console::Reset();
}