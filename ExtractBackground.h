#pragma once
#include "resource.h"
#include <windows.h>
#include <shellapi.h>
#include <ShlObj.h>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <filesystem>

// ── Fixed identifiers for the AudioCopy Windows Terminal profile ────────────
inline constexpr const char* AudioCopyProfileGuid =
"{a0d1c0e2-5b8f-4c3a-9e7d-6f2b1a8c4d5e}";

// Desired background opacity (0.0 = transparent, 1.0 = opaque)
inline constexpr const char* DesiredOpacity = "0.6";

// ── Convert wide string to UTF-8 safely ─────────────────────────────────────
inline std::string WideToUtf8(const std::wstring& wide) {
	if (wide.empty()) return {};
	int size = WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), -1, nullptr, 0, nullptr, nullptr);
	if (size <= 0) return {};
	std::string utf8(size - 1, '\0');
	WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), -1, utf8.data(), size, nullptr, nullptr);
	return utf8;
}

// ── Convert a file path to UTF-8 with JSON-safe forward slashes ─────────────
inline std::string PathToJsonUtf8(const std::wstring& path) {
	std::string utf8 = WideToUtf8(path);
	for (auto& c : utf8) if (c == '\\') c = '/';
	return utf8;
}

// ── Get the running executable's full path ──────────────────────────────────
inline std::wstring GetExePath() {
	wchar_t buf[MAX_PATH];
	GetModuleFileNameW(nullptr, buf, MAX_PATH);
	return buf;
}

// ── Extract embedded PNG to a permanent location ────────────────────────────
// Writes to %LOCALAPPDATA%\AudioCopy\background.png (survives temp cleanup).
inline std::wstring ExtractBackgroundImage() {
	HRSRC hRes = FindResourceW(nullptr, MAKEINTRESOURCEW(IDR_BACKGROUND_PNG), RT_RCDATA);
	if (!hRes) return {};

	HGLOBAL hData = LoadResource(nullptr, hRes);
	if (!hData) return {};

	void* pData = LockResource(hData);
	DWORD size = SizeofResource(nullptr, hRes);
	if (!pData || size == 0) return {};

	// Build path: %LOCALAPPDATA%\AudioCopy\background.png
	wchar_t localAppData[MAX_PATH];
	if (FAILED(SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA, nullptr, 0, localAppData)))
		return {};

	std::wstring dir = std::wstring(localAppData) + L"\\AudioCopy";
	std::filesystem::create_directories(dir);
	std::wstring path = dir + L"\\background.png";

	// Re-extract if missing or if embedded size differs (image was updated)
	bool needsWrite = !std::filesystem::exists(path)
		|| std::filesystem::file_size(path) != static_cast<uintmax_t>(size);

	if (needsWrite) {
		std::ofstream out(path, std::ios::binary);
		if (!out) return {};
		out.write(static_cast<const char*>(pData), size);
		out.close();
	}

	return path;
}

// ── Locate Windows Terminal settings.json ───────────────────────────────────
// Checks both the Store (packaged) and scoop/standalone (unpackaged) paths.
inline std::wstring FindTerminalSettings() {
	wchar_t localAppData[MAX_PATH];
	if (FAILED(SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA, nullptr, 0, localAppData)))
		return {};

	// Store / MSIX install (most common)
	std::wstring storePath = std::wstring(localAppData)
		+ L"\\Packages\\Microsoft.WindowsTerminal_8wekyb3d8bbwe\\LocalState\\settings.json";
	if (std::filesystem::exists(storePath))
		return storePath;

	// Unpackaged / scoop / standalone install
	std::wstring standalonePath = std::wstring(localAppData)
		+ L"\\Microsoft\\Windows Terminal\\settings.json";
	if (std::filesystem::exists(standalonePath))
		return standalonePath;

	return {};
}

// ── Check whether this process is running inside Windows Terminal ────────────
inline bool IsInWindowsTerminal() {
	wchar_t val[64]{};
	return GetEnvironmentVariableW(L"WT_SESSION", val, 64) > 0;
}

// ── Check whether the current tab uses the AudioCopy profile ────────────────
inline bool IsInAudioCopyProfile() {
	wchar_t val[256]{};
	if (GetEnvironmentVariableW(L"WT_PROFILE_ID", val, 256) == 0)
		return false;
	return WideToUtf8(val).find("a0d1c0e2-5b8f-4c3a-9e7d-6f2b1a8c4d5e")
		!= std::string::npos;
}

// ── Remove old defaults-based injection (migration) ─────────────────────────
// Previous versions injected background properties directly into the
// "defaults" block, polluting every terminal session.  This strips those
// lines so defaults are left clean.  JSONC tolerates any trailing commas
// left behind.
inline bool MigrateOldDefaultsInjection(std::string& json) {
	size_t dpos = json.find("\"defaults\"");
	if (dpos == std::string::npos) return false;

	size_t dbrace = json.find('{', dpos);
	if (dbrace == std::string::npos) return false;

	// Find the matching closing brace of the defaults block
	int depth = 0;
	size_t dend = dbrace;
	for (size_t i = dbrace; i < json.size(); ++i) {
		if (json[i] == '{') ++depth;
		else if (json[i] == '}') { if (--depth == 0) { dend = i; break; } }
	}

	std::string defBlock = json.substr(dbrace + 1, dend - dbrace - 1);
	if (defBlock.find("AudioCopy") == std::string::npos) return false;

	// Split into lines, discard the ones we previously injected
	std::istringstream iss(defBlock);
	std::string line;
	std::string cleaned;
	bool removed = false;

	while (std::getline(iss, line)) {
		bool ours =
			(line.find("\"backgroundImage\":") != std::string::npos &&
				line.find("AudioCopy") != std::string::npos) ||
			(line.find("\"backgroundImageOpacity\":") != std::string::npos &&
				line.find(DesiredOpacity) != std::string::npos) ||
			(line.find("\"backgroundImageStretchMode\":") != std::string::npos &&
				line.find("uniformToFill") != std::string::npos) ||
			(line.find("\"background\":") != std::string::npos &&
				line.find("#24272B") != std::string::npos);

		if (ours) removed = true;
		else       cleaned += line + "\n";
	}

	if (removed)
		json.replace(dbrace + 1, dend - dbrace - 1, cleaned);

	return removed;
}

// ── Helper: write JSON string to disk ───────────────────────────────────────
inline bool WriteSettingsJson(const std::wstring& path, const std::string& json) {
	std::ofstream out(path);
	if (!out) return false;
	out << json;
	out.close();
	return true;
}

// ── Ensure the AudioCopy profile exists in profiles → list ──────────────────
// Adds a dedicated profile with the background image.  Never touches
// "defaults", so other terminal sessions are completely unaffected.
// Ensures a dedicated "AudioCopy" profile exists in Windows Terminal's
// settings.json, containing the branded background image, opacity, and
// commandline.  If an older version injected properties into "defaults"
// (polluting all tabs), that injection is migrated away first.
inline bool EnsureAudioCopyProfile(const std::wstring& imagePath) {
	std::wstring settingsPath = FindTerminalSettings();
	if (settingsPath.empty()) return false;  // WT not installed

	// Slurp the entire settings.json into memory for string surgery.
	// Windows Terminal uses JSONC (JSON with comments), so a full parser
	// isn't strictly required — targeted find/replace is sufficient.
	std::ifstream inFile(settingsPath);
	if (!inFile) return false;
	std::stringstream sbuf;
	sbuf << inFile.rdbuf();
	std::string json = sbuf.str();
	inFile.close();

	// Remove any legacy injection that was placed inside the "defaults"
	// block by earlier versions of this tool.
	bool migrated = MigrateOldDefaultsInjection(json);

	// Convert paths to UTF-8 with forward slashes for JSON compatibility.
	std::string imgUtf8 = PathToJsonUtf8(imagePath);
	std::string exeUtf8 = PathToJsonUtf8(GetExePath());

	// ── Profile already exists — update fields if they've drifted ───────
	// (e.g. the user moved the exe or a new version changed the image)
	if (json.find(AudioCopyProfileGuid) != std::string::npos) {
		bool changed = false;
		size_t gp = json.find(AudioCopyProfileGuid);

		// Scope all edits to the JSON object that contains the GUID so we
		// don't accidentally modify a different profile's keys.
		size_t profStart = json.rfind('{', gp);
		int depth = 0;
		size_t profEnd = profStart;
		for (size_t i = profStart; i < json.size(); ++i) {
			if (json[i] == '{') ++depth;
			else if (json[i] == '}') { if (--depth == 0) { profEnd = i; break; } }
		}

		// Generic helper: finds a JSON key within the profile block and
		// replaces its quoted string value if it doesn't match `desired`.
		auto updateQuoted = [&](const char* key, const std::string& desired) {
			std::string k = std::string("\"") + key + "\":";
			size_t kp = json.find(k, profStart);
			if (kp == std::string::npos || kp > profEnd) return;
			size_t vs = json.find('"', kp + k.size());
			if (vs == std::string::npos) return;
			++vs;
			size_t ve = json.find('"', vs);
			if (ve == std::string::npos) return;
			if (json.substr(vs, ve - vs) != desired) {
				size_t oldLen = ve - vs;
				json.replace(vs, oldLen, desired);
				profEnd += desired.size() - oldLen;
				changed = true;
			}
			};

		updateQuoted("backgroundImage", imgUtf8);  // Update image path
		updateQuoted("commandline", exeUtf8);       // Update exe path

		// Opacity is a bare number (not quoted), so it needs separate
		// find-and-replace logic.
		const std::string opKey = "\"backgroundImageOpacity\":";
		size_t opPos = json.find(opKey, profStart);
		if (opPos != std::string::npos && opPos < profEnd) {
			size_t vs = json.find_first_not_of(" \t", opPos + opKey.size());
			size_t ve = json.find_first_of(",\r\n}", vs);
			if (vs != std::string::npos && ve != std::string::npos) {
				if (json.substr(vs, ve - vs) != DesiredOpacity) {
					json.replace(vs, ve - vs, DesiredOpacity);
					changed = true;
				}
			}
		}

		// Only write the file if something actually changed.
		if (changed || migrated) {
			WriteSettingsJson(settingsPath, json);
		}
		return true;
	}

	// ── Fresh injection — profile doesn't exist yet ─────────────────────
	// Locate the "profiles" → "list" array (or bare "profiles" array for
	// older WT settings formats).
	size_t bracketPos = std::string::npos;
	size_t profilesPos = json.find("\"profiles\"");
	if (profilesPos == std::string::npos) {
		if (migrated) WriteSettingsJson(settingsPath, json);
		return false;
	}

	// Handle both { "profiles": { "list": [...] } } and { "profiles": [...] }
	size_t listPos = json.find("\"list\"", profilesPos);
	if (listPos != std::string::npos)
		bracketPos = json.find('[', listPos);
	else
		bracketPos = json.find('[', profilesPos);

	if (bracketPos == std::string::npos) {
		if (migrated) WriteSettingsJson(settingsPath, json);
		return false;
	}

	// Build the new profile JSON object with all AudioCopy-specific
	// settings: GUID, name, commandline, background image, opacity,
	// stretch mode, background colour, and visibility.
	std::string profile =
		"\n            {\n"
		"                \"guid\": \"" + std::string(AudioCopyProfileGuid) + "\",\n"
		"                \"name\": \"AudioCopy\",\n"
		"                \"commandline\": \"" + exeUtf8 + "\",\n"
		"                \"backgroundImage\": \"" + imgUtf8 + "\",\n"
		"                \"backgroundImageOpacity\": " + std::string(DesiredOpacity) + ",\n"
		"                \"backgroundImageStretchMode\": \"uniformToFill\",\n"
		"                \"background\": \"#24272B\",\n"
		"                \"hidden\": false\n"
		"            },";

	// Back up settings.json before the first modification so the user
	// can restore it if anything goes wrong.
	std::wstring backupPath = settingsPath + L".audiocopy.bak";
	if (!std::filesystem::exists(backupPath)) {
		std::error_code ec;
		std::filesystem::copy_file(settingsPath, backupPath, ec);
	}

	// Insert the new profile at the start of the list array.
	json.insert(bracketPos + 1, profile);
	return WriteSettingsJson(settingsPath, json);
}

// ── Remove the AudioCopy profile from Windows Terminal ──────────────────────
// Call during uninstall or from a cleanup menu option.
inline bool RemoveAudioCopyProfile() {
	std::wstring settingsPath = FindTerminalSettings();
	if (settingsPath.empty()) return false;

	std::ifstream inFile(settingsPath);
	if (!inFile) return false;
	std::stringstream sbuf;
	sbuf << inFile.rdbuf();
	std::string json = sbuf.str();
	inFile.close();

	size_t guidPos = json.find(AudioCopyProfileGuid);
	if (guidPos == std::string::npos) return true; // Already absent

	// Walk back to the opening '{' of the profile object
	size_t objStart = json.rfind('{', guidPos);
	if (objStart == std::string::npos) return false;

	// Walk forward to the matching '}'
	int depth = 0;
	size_t objEnd = objStart;
	for (size_t i = objStart; i < json.size(); ++i) {
		if (json[i] == '{') ++depth;
		else if (json[i] == '}') { if (--depth == 0) { objEnd = i; break; } }
	}

	size_t eraseStart = objStart;
	size_t eraseEnd = objEnd + 1;

	// Consume trailing comma if present (our injection always adds one)
	size_t next = json.find_first_not_of(" \t\r\n", eraseEnd);
	if (next != std::string::npos && json[next] == ',') {
		eraseEnd = next + 1;
	}
	else {
		// No trailing comma — consume a leading comma instead
		// (profile may have been moved to the end of the list)
		size_t prev = eraseStart;
		while (prev > 0 && (json[prev - 1] == ' ' || json[prev - 1] == '\t'
			|| json[prev - 1] == '\r' || json[prev - 1] == '\n'))
			--prev;
		if (prev > 0 && json[prev - 1] == ',')
			eraseStart = prev - 1;
	}

	// Trim leading whitespace / newline for clean formatting
	while (eraseStart > 0 &&
		(json[eraseStart - 1] == ' ' || json[eraseStart - 1] == '\t'))
		--eraseStart;
	if (eraseStart > 0 && json[eraseStart - 1] == '\n') --eraseStart;
	if (eraseStart > 0 && json[eraseStart - 1] == '\r') --eraseStart;

	json.erase(eraseStart, eraseEnd - eraseStart);

	if (!WriteSettingsJson(settingsPath, json))
		return false;

	// Clean up the backup file
	std::wstring backupPath = settingsPath + L".audiocopy.bak";
	std::error_code ec;
	std::filesystem::remove(backupPath, ec);

	return true;
}

// ── Relaunch inside the AudioCopy WT profile for background support ─────────
// If not already in the AudioCopy profile and Windows Terminal is available,
// opens a new WT window with the AudioCopy profile running this exe.
// Returns true if relaunch was initiated (caller should return 0 to exit).
inline bool RelaunchInAudioCopyProfile() {
	if (IsInAudioCopyProfile()) return false;          // Already styled
	if (FindTerminalSettings().empty()) return false;   // WT not installed

	std::wstring exe = GetExePath();
	std::wstring args = L"-p \"AudioCopy\" -- \"" + exe + L"\"";

	HINSTANCE result = ShellExecuteW(
		nullptr, L"open", L"wt.exe", args.c_str(), nullptr, SW_SHOWNORMAL);

	bool launched = reinterpret_cast<INT_PTR>(result) > 32;

	if (launched) {
		// Detach from the current console so the hosting terminal
		// (conhost or Windows Terminal) can close the tab/window.
		FreeConsole();
	}

	return launched;
}