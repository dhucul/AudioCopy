#include "CopyWorkflow.h"
#include "FileUtils.h"
#include "ConsoleColors.h"
#include "AccurateRip.h"
#include "Progress.h"
#include "MenuHelpers.h"
#include <windows.h>
#include <iostream>
#include <conio.h>

bool RunCopyWorkflow(AudioCDCopier& copier, DiscInfo& disc, const std::wstring& /*workDir*/) {
	Console::Info("\n(Enter 0 at any prompt to go back to menu)\n");

	if (disc.sessionCount > 1) {
		int session = copier.SelectSession(disc.sessionCount);
		if (session == -1) return false;
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
	if (secureMode == -1) return false;

	bool isBurstMode = (secureMode == -2);
	SecureRipConfig secureConfig{};
	if (!isBurstMode) {
		secureConfig = copier.GetSecureRipConfig(static_cast<SecureRipMode>(secureMode));
	}

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
	if (offset == -1) return false;
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

		int wlen = MultiByteToWideChar(CP_ACP, 0, narrowPath.c_str(), -1, nullptr, 0);
		if (wlen > 1) {
			path.resize(wlen - 1);
			MultiByteToWideChar(CP_ACP, 0, narrowPath.c_str(), -1, &path[0], wlen);
		}
		else {
			path.clear();
		}

		if (path == L"0") return false;

		path = NormalizePath(path);

		if (path.empty()) {
			Console::Error("Error: Empty path specified. Please try again.\n");
			continue;
		}

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

			ULARGE_INTEGER freeBytesAvailable;
			if (!GetDiskFreeSpaceExW(root.c_str(), &freeBytesAvailable, nullptr, nullptr)) {
				Console::Error("Error: Drive ");
				std::wcerr << driveLetter << L": is not accessible.\n";
				Console::Warning("Check if the drive is ready and you have permission.\n");
				continue;
			}

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

		std::wstring testDir = path;
		bool isDirectory = (!path.empty() && (path.back() == L'\\' || path.back() == L'/'));

		if (!isDirectory) {
			size_t lastSlash = path.find_last_of(L"\\/");
			if (lastSlash != std::wstring::npos) {
				testDir = path.substr(0, lastSlash);
				if (testDir.length() == 2 && testDir[1] == L':') {
					testDir += L"\\";
				}
			}
			else {
				testDir = L".";
			}
		}

		if (!testDir.empty() && testDir != L".") {
			DWORD attrs = GetFileAttributesW(testDir.c_str());
			if (attrs == INVALID_FILE_ATTRIBUTES) {
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
			CloseHandle(hTest);
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

	if (secureConfig.mode != SecureRipMode::Disabled && secureConfig.mode != SecureRipMode::Burst) {
		std::wstring secureLogPath = path + L"_secure.log";
		if (copier.SaveSecureRipLog(secureResult, secureLogPath)) {
			Console::Success("Secure rip log saved to: ");
			std::wcout << secureLogPath << L"\n";
		}
	}

	copier.Eject();
	Console::Success("\nComplete!\n");
	return true;
}