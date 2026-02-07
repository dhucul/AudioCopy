// ============================================================================
// main.cpp - Audio CD Copy Tool Entry Point
// ============================================================================

#define NOMINMAX

#include "AudioCDCopier.h"
#include "AccurateRip.h"
#include "InterruptHandler.h"
#include "MenuHelpers.h"
#include "Progress.h"
#include "ConsoleColors.h"
#include <windows.h>
#include <ntddcdrm.h>
#include <iostream>
#include <conio.h>
#include <algorithm>  // for std::find

constexpr int DRIVE_POLL_INTERVAL_MS = 500;
constexpr DWORD AUDIO_TRACK_MASK = 0x04;

// ============================================================================
// Helper Functions
// ============================================================================

void CenterConsoleWindow() {
	Sleep(100);  // Allow window to fully initialize

	HWND consoleWnd = GetConsoleWindow();
	if (!consoleWnd) return;

	// For Windows Terminal, we need to find the actual window (parent of console)
	HWND targetWnd = consoleWnd;
	HWND parent = GetAncestor(consoleWnd, GA_ROOTOWNER);
	if (parent && parent != consoleWnd) {
		targetWnd = parent;  // Use Windows Terminal window
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

void WaitForKey(const char* message = "\nPress any key to return to menu...\n") {
	Console::Info(message);
	_getch();
}

void PrintMenuItem(int num, const char* text, bool dimmed = false) {
	Console::SetColor(dimmed ? Console::Color::DarkGray : Console::Color::Cyan);
	std::cout << (num < 10 ? " " : "") << num << ".";
	Console::Reset();
	std::cout << " " << text << "\n";
}

std::wstring GetWorkingDirectory() {
	std::wstring dir(MAX_PATH, L'\0');
	DWORD len = GetModuleFileNameW(nullptr, &dir[0], static_cast<DWORD>(dir.size()));

	if (len == 0) {
		Console::Warning("Failed to get module path, using current directory\n");
		dir.resize(MAX_PATH);
		len = GetCurrentDirectoryW(static_cast<DWORD>(dir.size()), &dir[0]);
		if (len == 0) {
			return L".";
		}
		dir.resize(len);
		return dir;
	}

	DWORD lastError = GetLastError();
	if (len == 0 || (len >= dir.size() - 1 && lastError == ERROR_INSUFFICIENT_BUFFER)) {
		dir.resize(32767);
		len = GetModuleFileNameW(nullptr, &dir[0], static_cast<DWORD>(dir.size()));
		if (len == 0 || len >= dir.size() - 1) {
			Console::Warning("Path too long, using current directory\n");
			return L".";
		}
	}

	dir.resize(len);
	size_t pos = dir.find_last_of(L"\\/");
	return (pos != std::wstring::npos) ? dir.substr(0, pos) : dir;
}

// Return values:
//  -1 => No disc / storage check failed
//  -2 => TOC read failed (empty/blank or unreadable TOC)
//  >=0 => number of audio tracks found
int GetAudioTrackCount(HANDLE h) {
	DWORD ret;
	if (!DeviceIoControl(h, IOCTL_STORAGE_CHECK_VERIFY, nullptr, 0, nullptr, 0, &ret, nullptr))
		return -1;

	CDROM_TOC toc = {};
	if (!DeviceIoControl(h, IOCTL_CDROM_READ_TOC, nullptr, 0, &toc, sizeof(toc), &ret, nullptr))
		return -2;

	int n = toc.LastTrack - toc.FirstTrack + 1;
	int audioTracks = 0;
	for (int i = 0; i < n; i++) {
		if ((toc.TrackData[i].Control & AUDIO_TRACK_MASK) == 0) audioTracks++;
	}
	return audioTracks;
}

bool WaitForMediaReady(HANDLE h, int maxWaitMs = 5000) {
	DWORD startTime = GetTickCount();
	while (GetTickCount() - startTime < static_cast<DWORD>(maxWaitMs)) {
		DWORD ret;
		if (DeviceIoControl(h, IOCTL_STORAGE_CHECK_VERIFY, nullptr, 0, nullptr, 0, &ret, nullptr)) {
			return true;
		}
		DWORD err = GetLastError();
		if (err == ERROR_NOT_READY) {
			Sleep(250);
			continue;
		}
		if (err == ERROR_MEDIA_CHANGED) {
			continue;
		}
		break;
	}
	return false;
}

bool CheckForAudioTracks(HANDLE h) {
	int count = GetAudioTrackCount(h);
	return count > 0;
}

std::string GetDriveName(HANDLE h) {
	STORAGE_PROPERTY_QUERY query = {};
	query.PropertyId = StorageDeviceProperty;
	query.QueryType = PropertyStandardQuery;

	BYTE buffer[1024] = {};
	DWORD ret;
	if (!DeviceIoControl(h, IOCTL_STORAGE_QUERY_PROPERTY, &query, sizeof(query),
		buffer, sizeof(buffer), &ret, nullptr)) {
		return "CD/DVD drive";
	}

	auto* desc = reinterpret_cast<STORAGE_DEVICE_DESCRIPTOR*>(buffer);
	std::string name;

	auto appendTrimmed = [&](DWORD offset) {
		if (offset && buffer[offset]) {
			std::string part = reinterpret_cast<char*>(buffer + offset);
			while (!part.empty() && part.back() == ' ')
				part.pop_back();
			if (!part.empty()) {
				if (!name.empty()) name += " ";
				name += part;
			}
		}
		};

	appendTrimmed(desc->VendorIdOffset);
	appendTrimmed(desc->ProductIdOffset);

	return name.empty() ? "CD/DVD drive" : name;
}

HANDLE OpenDriveHandle(wchar_t letter) {
	std::wstring devPath = L"\\\\.\\" + std::wstring(1, letter) + L":";
	return CreateFileW(devPath.c_str(), GENERIC_READ,
		FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
}

std::vector<wchar_t> ScanDrives(wchar_t& audioDrive) {
	std::vector<wchar_t> cdDrives;
	audioDrive = 0;
	DWORD driveMask = GetLogicalDrives();

	for (wchar_t letter = L'A'; letter <= L'Z'; letter++) {
		if (!(driveMask & (1 << (letter - L'A'))))
			continue;

		std::wstring root = std::wstring(1, letter) + L":\\";
		if (GetDriveTypeW(root.c_str()) != DRIVE_CDROM)
			continue;

		cdDrives.push_back(letter);
		std::cout << "  [";
		Console::SetColor(Console::Color::Yellow);
		std::cout << static_cast<char>(letter) << ":";
		Console::Reset();
		std::cout << "] ";

		HANDLE h = OpenDriveHandle(letter);
		if (h == INVALID_HANDLE_VALUE) {
			std::cout << "\n";
			continue;
		}

		std::cout << GetDriveName(h);

		int audioTracks = GetAudioTrackCount(h);
		if (audioTracks == -1) {
			Console::SetColor(Console::Color::DarkGray);
			std::cout << " - No disc";
			Console::Reset();
		}
		else if (audioTracks == -2) {
			Console::SetColor(Console::Color::DarkGray);
			std::cout << " - Empty/Blank";
			Console::Reset();
		}
		else if (audioTracks > 0) {
			std::cout << " - ";
			Console::SetColor(Console::Color::Green);
			std::cout << "AUDIO CD (" << audioTracks << " tracks)";
			Console::Reset();
			if (!audioDrive) audioDrive = letter;
		}
		else {
			std::cout << " - Data disc";
		}

		CloseHandle(h);
		std::cout << "\n";
	}

	return cdDrives;
}

// Returns 0 if user cancelled, otherwise returns the drive letter
wchar_t WaitForDisc(const std::vector<wchar_t>& cdDrives, int timeoutSeconds = 0) {
	Console::Warning("\nNo audio CD detected. Insert disc or enter drive letter (ESC to cancel, Enter to wait): ");

	const DWORD startTime = GetTickCount();
	const DWORD timeoutMs = (timeoutSeconds > 0) ? static_cast<DWORD>(timeoutSeconds) * 1000 : 0;
	int lastSecondsRemaining = -1;

	while (true) {
		if (timeoutMs > 0) {
			DWORD elapsed = GetTickCount() - startTime;
			if (elapsed >= timeoutMs) {
				Console::Warning("\nTimeout waiting for disc.\n");
				return 0;
			}
			int secondsRemaining = static_cast<int>((timeoutMs - elapsed) / 1000);
			if (secondsRemaining != lastSecondsRemaining) {
				lastSecondsRemaining = secondsRemaining;
				std::cout << "\rWaiting... " << secondsRemaining << "s remaining (ESC to cancel)   ";
			}
		}

		if (_kbhit()) {
			char c = _getch();
			if (c == 27) {
				std::cout << "\n";
				Console::Warning("Cancelled by user.\n");
				return 0;
			}

			if (c == '\r' || c == '\n') {
				std::cout << "\nWaiting for disc insertion... (ESC to cancel, or press drive letter)\n";
			}
			else if (isalpha(c)) {
				wchar_t entered = towupper(c);
				if (std::find(cdDrives.begin(), cdDrives.end(), entered) != cdDrives.end()) {
					std::cout << c << "\n";
					return entered;
				}
				std::cout << c << " - Not a CD drive\n";
			}
		}

		if (g_interrupt.IsInterrupted()) {
			Console::Warning("\nInterrupted.\n");
			return 0;
		}

		for (wchar_t letter : cdDrives) {
			HANDLE h = OpenDriveHandle(letter);
			if (h != INVALID_HANDLE_VALUE) {
				if (WaitForMediaReady(h, 2000) && CheckForAudioTracks(h)) {
					CloseHandle(h);
					Console::Info("\nAudio CD detected in drive ");
					std::cout << static_cast<char>(letter) << ":\n";
					return letter;
				}
				CloseHandle(h);
			}
		}

		Sleep(DRIVE_POLL_INTERVAL_MS);
	}
}

bool CreateDirectoryRecursive(const std::wstring& path) {
	std::wstring workPath = path;
	if (path.length() > MAX_PATH - 12 && path.substr(0, 4) != L"\\\\?\\") {
		if (path.length() >= 2 && path[1] == L':') {
			workPath = L"\\\\?\\" + path;
		}
		else if (path.substr(0, 2) == L"\\\\") {
			workPath = L"\\\\?\\UNC\\" + path.substr(2);
		}
	}

	size_t startPos = 0;
	if (workPath.substr(0, 4) == L"\\\\?\\") {
		startPos = 4;
		if (workPath.substr(4, 4) == L"UNC\\") {
			startPos = 8;
		}
	}

	size_t pos = startPos;
	while ((pos = workPath.find_first_of(L"\\/", pos + 1)) != std::wstring::npos) {
		std::wstring subPath = workPath.substr(0, pos);
		DWORD attrs = GetFileAttributesW(subPath.c_str());
		if (attrs == INVALID_FILE_ATTRIBUTES) {
			if (!CreateDirectoryW(subPath.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS)
				return false;
		}
	}
	DWORD attrs = GetFileAttributesW(workPath.c_str());
	if (attrs == INVALID_FILE_ATTRIBUTES) {
		return CreateDirectoryW(workPath.c_str(), nullptr) || GetLastError() == ERROR_ALREADY_EXISTS;
	}
	return true;
}

std::wstring SanitizeFilename(const std::wstring& name) {
	std::wstring result = name;
	const wchar_t* invalid = L"<>:\"/\\|?*";
	for (wchar_t& c : result) {
		if (wcschr(invalid, c)) c = L'_';
		if (c < 0x20) c = L'_';
	}

	while (!result.empty() && (result.back() == L' ' || result.back() == L'.')) {
		result.pop_back();
	}

	if (result.empty()) {
		result = L"AudioCD";
	}

	return result;
}

// Trim and normalize a path string
std::wstring NormalizePath(const std::wstring& path) {
	std::wstring result = path;
	while (!result.empty() && (result.front() == L' ' || result.front() == L'\t'))
		result.erase(0, 1);
	while (!result.empty() && (result.back() == L' ' || result.back() == L'\t'))
		result.pop_back();
	return result;
}

// ============================================================================
// Help Menu
// ============================================================================

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
		{"16. C2 Validation Test",  // NEW
		 "Tests the reliability of your drive's C2 error reporting at different speeds.\n"
		 "   Some drives report false C2 errors at high speeds. This test verifies accuracy\n"
		 "   by comparing C2 results at slow and fast speeds for consistency.",
		 "Determining if your drive's C2 detection is trustworthy before scanning."},
		{"17. Exit",  // CHANGED from 16
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

// ============================================================================
// Copy Workflow
// ============================================================================

bool RunCopyWorkflow(AudioCDCopier& copier, DiscInfo& disc, const std::wstring& workDir) {
	Console::Info("\n(Enter 0 at any prompt to go back to menu)\n");

	if (disc.sessionCount > 1) {
		int session = copier.SelectSession(disc.sessionCount);
		if (session == -1) return false;  // ✓ FIXED: Check for back to menu
		disc.selectedSession = session;
	}

	int speed = copier.SelectSpeed();
	if (speed == -1) return false;

	int subch = copier.SelectSubchannel();
	if (subch == -1) return false;
	disc.includeSubchannel = (subch == 1);

	int pregapMode = copier.SelectPregapMode();
	if (pregapMode == -1) return false;
	disc.pregapMode = static_cast<PregapMode>(pregapMode);

	int errorMode = copier.SelectErrorHandling();
	if (errorMode == -1) return false;

	int secureMode = copier.SelectSecureRipMode();
	if (secureMode == -1) return false;  // ✓ FIXED: Changed from INT_MIN to -1

	SecureRipConfig secureConfig = copier.GetSecureRipConfig(static_cast<SecureRipMode>(secureMode));
	bool isBurstMode = (secureMode == -2);  // ✓ FIXED: Changed from -1 to -2 (BURST mode)

	disc.loggingOutput = LogOutput::File;

	if (!isBurstMode) {
		DriveCapabilities caps;
		if (copier.DetectDriveCapabilities(caps)) {
			disc.enableC2Detection = caps.supportsC2ErrorReporting;
		}
		else {
			disc.enableC2Detection = false;
		}
	}
	else {
		disc.enableC2Detection = false;
	}

	int offset = copier.SelectOffset();
	if (offset == -1) return false;  // ✓ FIXED: Changed from INT_MIN to -1
	disc.driveOffset = offset;

	int cacheDefeat = copier.SelectCacheDefeat();
	if (cacheDefeat == -1) return false;
	disc.enableCacheDefeat = (cacheDefeat == 1);

	std::wstring path;
	while (true) {
		std::cout << "\nOutput path (no extension, or 0 to go back):\n";
		Console::SetColor(Console::Color::DarkGray);
		std::cout << "  Examples: C:\\Music\\MyAlbum  or  D:\\Rips\\  or  .\\output\n";
		Console::Reset();
		std::cout << "Path: ";

		std::string narrowPath;
		std::getline(std::cin, narrowPath);
		path = std::wstring(narrowPath.begin(), narrowPath.end());

		if (path == L"0") return false;

		path = NormalizePath(path);

		if (path.empty()) {
			Console::Error("Error: Empty path specified. Please try again.\n");
			continue;
		}

		// Check for valid characters
		bool hasValidChar = false;
		bool hasInvalidChar = false;
		const wchar_t* invalidChars = L"<>\"|?*";
		for (wchar_t c : path) {
			if (iswalnum(c) || c == L'\\' || c == L'/' || c == L':' || c == L'.' || c == L'_' || c == L'-' || c == L' ') {
				hasValidChar = true;
			}
			if (wcschr(invalidChars, c)) {
				hasInvalidChar = true;
			}
		}

		if (hasInvalidChar) {
			Console::Error("Error: Path contains invalid characters (<>\"|?*).\n");
			continue;
		}

		if (!hasValidChar) {
			Console::Error("Error: Path contains only invalid characters.\n");
			Console::Warning("Valid example: C:\\Music\\MyAlbum\n");
			continue;
		}

		// Check drive letter validity and existence
		if (path.length() >= 2 && path[1] == L':') {
			wchar_t driveLetter = towupper(path[0]);
			if (driveLetter < L'A' || driveLetter > L'Z') {
				Console::Error("Error: Invalid drive letter.\n");
				Console::Warning("Valid example: C:\\Music\\MyAlbum\n");
				continue;
			}

			std::wstring root = std::wstring(1, driveLetter) + L":\\";
			UINT driveType = GetDriveTypeW(root.c_str());
			if (driveType == DRIVE_NO_ROOT_DIR || driveType == DRIVE_UNKNOWN) {
				Console::Error("Error: Drive ");
				std::wcerr << driveLetter << L": does not exist.\n";
				Console::Warning("Please enter a valid path or 0 to go back.\n");
				continue;
			}

			// Check if drive is ready and accessible
			ULARGE_INTEGER freeBytesAvailable;
			if (!GetDiskFreeSpaceExW(root.c_str(), &freeBytesAvailable, nullptr, nullptr)) {
				Console::Error("Error: Drive ");
				std::wcerr << driveLetter << L": is not accessible.\n";
				Console::Warning("Check if the drive is ready and you have permission.\n");
				continue;
			}

			// Warn if very low disk space (less than 1GB)
			if (freeBytesAvailable.QuadPart < 1073741824ULL) {
				Console::Warning("Warning: Low disk space on drive ");
				std::wcerr << driveLetter << L": (";
				std::wcerr << (freeBytesAvailable.QuadPart / (1024 * 1024)) << L" MB free)\n";
				std::cout << "Continue anyway? (y/n): ";
				char confirm = _getch();
				std::cout << confirm << "\n";
				if (tolower(confirm) != 'y') {
					continue;
				}
			}
		}

		// Determine parent directory to test write access
		std::wstring testDir = path;
		bool isDirectory = (!path.empty() && (path.back() == L'\\' || path.back() == L'/'));

		if (!isDirectory) {
			size_t lastSlash = path.find_last_of(L"\\/");
			if (lastSlash != std::wstring::npos) {
				testDir = path.substr(0, lastSlash);
				// Handle drive root case: "C:" -> "C:\"
				if (testDir.length() == 2 && testDir[1] == L':') {
					testDir += L"\\";
				}
			}
			else {
				testDir = L".";
			}
		}

		// Try to create or verify the directory exists and is writable
		if (!testDir.empty() && testDir != L".") {
			// Check if directory exists
			DWORD attrs = GetFileAttributesW(testDir.c_str());
			if (attrs == INVALID_FILE_ATTRIBUTES) {
				// Directory doesn't exist, try to create it
				if (!CreateDirectoryRecursive(testDir)) {
					DWORD err = GetLastError();
					Console::Error("Error: Cannot create directory: ");
					std::wcerr << testDir << L"\n";
					if (err == ERROR_ACCESS_DENIED) {
						Console::Warning("Access denied. Check permissions or run as administrator.\n");
					}
					else if (err == ERROR_PATH_NOT_FOUND) {
						Console::Warning("Part of the path does not exist.\n");
					}
					continue;
				}
				Console::Success("Created directory: ");
				std::wcout << testDir << L"\n";
			}
			else if (!(attrs & FILE_ATTRIBUTE_DIRECTORY)) {
				Console::Error("Error: Path exists but is not a directory: ");
				std::wcerr << testDir << L"\n";
				continue;
			}

			// Test write access by creating a temporary file
			std::wstring testFile = testDir + L"\\.audiocopy_test_" + std::to_wstring(GetTickCount()) + L".tmp";
			HANDLE hTest = CreateFileW(testFile.c_str(), GENERIC_WRITE, 0, nullptr,
				CREATE_NEW, FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
			if (hTest == INVALID_HANDLE_VALUE) {
				DWORD err = GetLastError();
				Console::Error("Error: Cannot write to directory: ");
				std::wcerr << testDir << L"\n";
				if (err == ERROR_ACCESS_DENIED) {
					Console::Warning("Access denied. Check folder permissions.\n");
				}
				continue;
			}
			CloseHandle(hTest);  // File auto-deleted due to DELETE_ON_CLOSE
		}

		Console::Success("Path validated successfully.\n");
		break;
	}
	bool isDirectory = (!path.empty() && (path.back() == L'\\' || path.back() == L'/'));
	if (!isDirectory) {
		DWORD attrs = GetFileAttributesW(path.c_str());
		if (attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY)) {
			isDirectory = true;
		}
		else {
			size_t lastSlash = path.find_last_of(L"\\/");
			std::wstring lastComponent = (lastSlash != std::wstring::npos) ? path.substr(lastSlash + 1) : path;
			if (lastComponent.find(L'.') == std::wstring::npos) {
				isDirectory = true;
			}
		}
	}

	if (isDirectory) {
		std::wstring defaultName = L"AudioCD";
		if (!disc.cdText.albumTitle.empty()) {
			std::string title = disc.cdText.albumTitle;
			if (!disc.cdText.albumArtist.empty()) {
				title = disc.cdText.albumArtist + " - " + title;
			}

			int len = MultiByteToWideChar(CP_UTF8, 0, title.c_str(), -1, nullptr, 0);
			std::wstring wideTitle(len - 1, L'\0');
			MultiByteToWideChar(CP_UTF8, 0, title.c_str(), -1, &wideTitle[0], len);
			defaultName = SanitizeFilename(wideTitle);
		}

		if (!path.empty() && path.back() != L'\\' && path.back() != L'/') {
			path += L"\\";
		}

		if (!CreateDirectoryRecursive(path)) {
			Console::Error("Failed to create directory: ");
			std::wcerr << path << L"\n";
			return false;
		}

		path += defaultName;
		std::wcout << L"Using filename: " << path << L"\n";
	}
	else {
		size_t lastSlash = path.find_last_of(L"\\/");
		if (lastSlash != std::wstring::npos) {
			std::wstring parentDir = path.substr(0, lastSlash);
			if (!CreateDirectoryRecursive(parentDir)) {
				Console::Error("Failed to create directory: ");
				std::wcerr << parentDir << L"\n";
				return false;
			}
		}
	}

	Console::Info("\nReading disc...\n");
	ProgressIndicator prog;
	prog.SetLabel("Reading");
	prog.Start();

	bool readSuccess = false;
	SecureRipResult secureResult;

	if (isBurstMode) {
		readSuccess = copier.ReadDiscBurst(disc, MakeProgressCallback(&prog));
	}
	else if (secureConfig.mode != SecureRipMode::Disabled) {
		readSuccess = copier.ReadDiscSecure(disc, secureConfig, secureResult,
			MakeProgressCallback(&prog));
	}
	else {
		readSuccess = copier.ReadDisc(disc, errorMode, MakeProgressCallback(&prog));
	}

	if (!readSuccess) {
		prog.Finish(false);
		return false;
	}
	prog.Finish(true);

	AccurateRip::VerifyCRCs(disc);
	if (disc.driveOffset != 0) {
		copier.ApplyOffsetCorrection(disc);
	}

	Console::Info("Saving files...\n");
	if (!copier.SaveToFile(disc, path)) {
		Console::Error("Failed to save!\n");
		return false;
	}

	std::wstring logPath = path + L".log";
	if (copier.SaveReadLog(disc, logPath)) {
		Console::Success("Log saved to: ");
		std::wcout << logPath << L"\n";
	}

	copier.Eject();
	Console::Success("\nComplete!\n");
	return true;
}

// ============================================================================
// Main Entry Point
// ============================================================================

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
		audioDrive = WaitForDisc(cdDrives, 0);  // 0 = no timeout
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
		PrintMenuItem(16, "C2 validation test");  // NEW
		PrintMenuItem(17, "Exit", true);  // CHANGED from 16
		std::cout << "Choice: ";

		int choice = GetMenuChoice(1, 17, 1);  // CHANGED from 16
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

		case 16: {  // NEW - C2 Validation Test
			Console::Info("\n=== C2 Validation Test ===\n");
			Console::Info("This test reads a sector at different speeds to verify C2 accuracy.\n");
			Console::Info("Inconsistent C2 results may indicate unreliable C2 reporting.\n\n");

			// Select a test sector from middle of first track
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

			// Call the validation test - need to add wrapper in AudioCDCopier
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

		case 17:  // CHANGED from 16
			Console::Success("\nGoodbye!\n");
			return 0;
		}

		if (choice != 17) {  // CHANGED from 16
			WaitForKey();
		}
	}
}

std::string GetDiscStatus(HANDLE h, bool& hasAudio, int& audioTracks) {
	hasAudio = false;
	audioTracks = 0;

	int count = GetAudioTrackCount(h);
	if (count == -1) return "No disc";
	if (count == -2) return "Empty/Blank";

	audioTracks = count;
	if (audioTracks > 0) {
		hasAudio = true;
		return "AUDIO CD (" + std::to_string(audioTracks) + " tracks)";
	}
	return "Data disc";
}