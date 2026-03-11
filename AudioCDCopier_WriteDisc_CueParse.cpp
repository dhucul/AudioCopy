#define NOMINMAX
#include "AudioCDCopier.h"
#include "ConsoleColors.h"
#include "WriteDiscInternal.h"
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <windows.h>

// ============================================================================
// Helper: Convert wide string to UTF-8 string (Windows API)
// ============================================================================
static std::string WideToUTF8(const std::wstring& wide) {
	if (wide.empty()) return std::string();
	int requiredSize = WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), -1, nullptr, 0, nullptr, nullptr);
	if (requiredSize == 0) return std::string();

	std::string utf8(requiredSize - 1, '\0');
	WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), -1, &utf8[0], requiredSize, nullptr, nullptr);
	return utf8;
}

// ============================================================================
// Helper: Trim trailing whitespace and carriage returns from a wide string
// ============================================================================
static void TrimTrailing(std::wstring& s) {
	size_t end = s.find_last_not_of(L" \t\r\n");
	if (end != std::wstring::npos)
		s.erase(end + 1);
	else
		s.clear();
}

// ============================================================================
// ParseCueSheet - Parse CUE file to extract track information with pregaps
// ============================================================================
bool AudioCDCopier::ParseCueSheet(const std::wstring& cueFile,
	std::vector<TrackWriteInfo>& tracks) {
	std::string discTitle, discPerformer, discMCN;
	return ParseCueSheet(cueFile, tracks, discTitle, discPerformer, discMCN);
}

bool AudioCDCopier::ParseCueSheet(const std::wstring& cueFile,
	std::vector<TrackWriteInfo>& tracks,
	std::string& discTitle, std::string& discPerformer,
	std::string& discMCN) {

	std::wifstream file(cueFile);
	if (!file.is_open()) {
		Console::Error("Cannot open CUE file: ");
		std::wcout << cueFile << L"\n";
		return false;
	}

	discTitle.clear();
	discPerformer.clear();
	discMCN.clear();
	tracks.clear();

	std::wstring line;
	TrackWriteInfo currentTrack = {};
	currentTrack.hasPregap = false;
	bool inTrack = false;
	int fileCount = 0;

	auto extractQuoted = [](const std::wstring& ln, const std::wstring& keyword) -> std::string {
		size_t pos = ln.find(keyword);
		if (pos == std::wstring::npos) return {};
		size_t q1 = ln.find(L'"', pos + keyword.size());
		if (q1 == std::wstring::npos) return {};
		size_t q2 = ln.find(L'"', q1 + 1);
		if (q2 == std::wstring::npos) return {};
		std::wstring val = ln.substr(q1 + 1, q2 - q1 - 1);
		return WideToUTF8(val);
		};

	while (std::getline(file, line)) {
		// Trim leading whitespace
		size_t start = line.find_first_not_of(L" \t");
		if (start == std::wstring::npos) continue;
		line = line.substr(start);

		// Trim trailing whitespace and stray CR (handles mixed line endings)
		TrimTrailing(line);
		if (line.empty()) continue;

		if (line.find(L"FILE") == 0) {
			fileCount++;
			if (fileCount > 1) {
				Console::Error("Multi-file CUE sheets are not supported\n");
				file.close();
				tracks.clear();
				return false;
			}
		}
		else if (line.find(L"CATALOG") == 0 && !inTrack) {
			// CATALOG specifies the disc's 13-digit Media Catalog Number (EAN/UPC)
			std::wistringstream iss(line);
			std::wstring cmd, catalog;
			iss >> cmd >> catalog;
			discMCN = WideToUTF8(catalog);
		}
		else if (line.find(L"TRACK") == 0) {
			if (inTrack && currentTrack.trackNumber > 0) {
				tracks.push_back(currentTrack);
			}
			inTrack = true;
			currentTrack = {};
			currentTrack.hasPregap = false;
			currentTrack.dataMode = 0;

			if (line.find(L"AUDIO") != std::wstring::npos) {
				currentTrack.isAudio = true;
				currentTrack.dataMode = 0;
			}
			else if (line.find(L"MODE1/2352") != std::wstring::npos) {
				currentTrack.isAudio = false;
				currentTrack.dataMode = 1;
			}
			else if (line.find(L"MODE2/2352") != std::wstring::npos) {
				currentTrack.isAudio = false;
				currentTrack.dataMode = 2;
			}
			else {
				Console::Warning("Unsupported track mode in CUE: ");
				std::wcout << line << L"\n";
				Console::Info("Only AUDIO, MODE1/2352, and MODE2/2352 are supported\n");
				tracks.clear();
				file.close();
				return false;
			}

			std::wistringstream iss(line);
			std::wstring cmd;
			iss >> cmd >> currentTrack.trackNumber;
		}
		else if (line.find(L"TITLE") == 0) {
			std::string val = extractQuoted(line, L"TITLE");
			if (!val.empty()) {
				if (inTrack)
					currentTrack.title = val;
				else
					discTitle = val;
			}
		}
		else if (line.find(L"PERFORMER") == 0) {
			std::string val = extractQuoted(line, L"PERFORMER");
			if (!val.empty()) {
				if (inTrack)
					currentTrack.performer = val;
				else
					discPerformer = val;
			}
		}
		else if (line.find(L"INDEX") == 0 && inTrack) {
			int indexNum = 0;
			std::wstring timeStr;
			std::wistringstream iss(line);
			std::wstring cmd;
			iss >> cmd >> indexNum >> timeStr;

			// Ignore INDEX 00 for data tracks (pregap is not applicable)
			if (!currentTrack.isAudio && indexNum == 0) continue;

			int mm = 0, ss = 0, ff = 0;
			wchar_t sep;
			std::wistringstream tss(timeStr);
			tss >> mm >> sep >> ss >> sep >> ff;

			DWORD lba = mm * 60 * 75 + ss * 75 + ff;

			if (indexNum == 0) {
				currentTrack.pregapLBA = lba;
				currentTrack.hasPregap = true;
			}
			else if (indexNum == 1) {
				currentTrack.startLBA = lba;
			}
		}
		else if (line.find(L"ISRC") == 0 && inTrack) {
			std::wistringstream iss(line);
			std::wstring cmd, isrc;
			iss >> cmd >> isrc;
			currentTrack.isrcCode = WideToUTF8(isrc);
		}
		else if (line.find(L"PREGAP") == 0 && inTrack) {
			// PREGAP generates silence not present in the BIN file.
			// This is different from INDEX 00, which marks an existing region.
			Console::Warning("PREGAP command detected but not supported (track ");
			std::cout << currentTrack.trackNumber
				<< ") -- only INDEX 00 pregaps are handled\n";
		}
		else if (line.find(L"POSTGAP") == 0 && inTrack) {
			Console::Warning("POSTGAP command detected but not supported (track ");
			std::cout << currentTrack.trackNumber << ")\n";
		}
		else if (line.find(L"FLAGS") == 0 && inTrack) {
			Console::Warning("FLAGS command detected but not written (track ");
			std::cout << currentTrack.trackNumber << ")\n";
		}
	}

	if (inTrack && currentTrack.trackNumber > 0) {
		tracks.push_back(currentTrack);
	}

	file.close();

	if (fileCount == 0) {
		Console::Error("No FILE directive found in CUE sheet\n");
		tracks.clear();
		return false;
	}

	// Validate parsed tracks
	if (tracks.empty()) {
		Console::Error("No tracks found in CUE sheet\n");
		return false;
	}

	for (size_t i = 0; i < tracks.size(); i++) {
		// Mixed-mode: data track must be track 1
		if (!tracks[i].isAudio && tracks[i].trackNumber != 1) {
			Console::Error("Data track must be track 1 in a mixed-mode disc (found at track ");
			std::cout << tracks[i].trackNumber << ")\n";
			tracks.clear();
			return false;
		}
		if (i > 0 && tracks[i].startLBA <= tracks[i - 1].startLBA) {
			Console::Error("Track ");
			std::cout << tracks[i].trackNumber
				<< " has non-increasing start LBA (expected > "
				<< tracks[i - 1].startLBA << ", got "
				<< tracks[i].startLBA << ")\n";
			tracks.clear();
			return false;
		}
		if (tracks[i].hasPregap && tracks[i].pregapLBA > tracks[i].startLBA) {
			Console::Warning("Track ");
			std::cout << tracks[i].trackNumber
				<< " has pregap LBA (" << tracks[i].pregapLBA
				<< ") after start LBA (" << tracks[i].startLBA
				<< ") -- ignoring pregap\n";
			tracks[i].hasPregap = false;
		}
	}

	// Compute endLBA for all tracks except the last.
	// The last track's endLBA must be set by the caller from the BIN file size.
	for (size_t i = 0; i < tracks.size(); i++) {
		if (i + 1 < tracks.size()) {
			tracks[i].endLBA = tracks[i + 1].hasPregap
				? tracks[i + 1].pregapLBA - 1
				: tracks[i + 1].startLBA - 1;
		}
	}

	// Enforce mandatory 150-frame transition gap between data track 1 and
	// the first audio track (Red Book / Yellow Book requirement).
	if (tracks.size() >= 2 && !tracks[0].isAudio && tracks[1].isAudio) {
		if (!tracks[1].hasPregap) {
			DWORD gapStart = (tracks[1].startLBA >= 150)
				? tracks[1].startLBA - 150
				: tracks[0].endLBA + 1;

			if (gapStart > tracks[0].startLBA) {
				tracks[1].pregapLBA = gapStart;
				tracks[1].hasPregap = true;
				Console::Info("Inserted mandatory 150-frame transition gap before audio track ");
				std::cout << tracks[1].trackNumber << " (LBA " << gapStart << ")\n";
			}
			else {
				Console::Warning("Cannot insert data-to-audio transition gap (insufficient space)\n");
			}
		}
	}

	Console::Success("Parsed CUE sheet: ");
	std::cout << tracks.size() << " tracks\n";

	if (!discTitle.empty() || !discPerformer.empty()) {
		Console::Info("CD-Text: ");
		if (!discPerformer.empty()) std::cout << discPerformer;
		if (!discPerformer.empty() && !discTitle.empty()) std::cout << " - ";
		if (!discTitle.empty()) std::cout << discTitle;
		std::cout << "\n";
	}

	for (const auto& t : tracks) {
		Console::Info("  Track ");
		std::cout << t.trackNumber << ": LBA " << t.startLBA
			<< " - " << t.endLBA;
		if (!t.isAudio) {
			const char* modeName = (t.dataMode == 2) ? "MODE2" : "MODE1";
			std::cout << " [" << modeName << "]";
		}
		if (t.hasPregap && t.startLBA >= t.pregapLBA) {
			std::cout << " (pregap at " << t.pregapLBA
				<< ", " << (t.startLBA - t.pregapLBA) << " frames)";
		}
		if (!t.title.empty()) {
			std::cout << " \"" << t.title << "\"";
		}
		std::cout << "\n";
	}

	return true;
}