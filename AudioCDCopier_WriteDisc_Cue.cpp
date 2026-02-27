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
// Helper: Wait for drive to become ready (poll TEST UNIT READY)
// ============================================================================
static bool WaitForDriveReady(ScsiDrive& drive, int timeoutSeconds) {
	for (int i = 0; i < timeoutSeconds * 4; i++) {
		BYTE testCmd[6] = { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };
		BYTE sk = 0, asc = 0, ascq = 0;
		if (drive.SendSCSIWithSense(testCmd, sizeof(testCmd), nullptr, 0,
			&sk, &asc, &ascq, true)) {
			return true;
		}
		if (sk == 0x02 && asc == 0x04) {
			Sleep(250);
			continue;
		}
		return false;
	}
	return false;
}

// ============================================================================
// ParseCueSheet - Parse CUE file to extract track information with pregaps
// ============================================================================
bool AudioCDCopier::ParseCueSheet(const std::wstring& cueFile,
	std::vector<TrackWriteInfo>& tracks) {
	std::string discTitle, discPerformer;
	return ParseCueSheet(cueFile, tracks, discTitle, discPerformer);
}

bool AudioCDCopier::ParseCueSheet(const std::wstring& cueFile,
	std::vector<TrackWriteInfo>& tracks,
	std::string& discTitle, std::string& discPerformer) {

	std::wifstream file(cueFile);
	if (!file.is_open()) {
		Console::Error("Cannot open CUE file: ");
		std::wcout << cueFile << L"\n";
		return false;
	}

	discTitle.clear();
	discPerformer.clear();

	std::wstring line;
	TrackWriteInfo currentTrack = {};
	currentTrack.hasPregap = false;
	bool inTrack = false;

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
		size_t start = line.find_first_not_of(L" \t");
		if (start == std::wstring::npos) continue;
		line = line.substr(start);

		if (line.find(L"TRACK") == 0) {
			if (inTrack && currentTrack.trackNumber > 0) {
				tracks.push_back(currentTrack);
			}
			inTrack = true;
			currentTrack = {};
			currentTrack.hasPregap = false;
			currentTrack.isAudio = (line.find(L"AUDIO") != std::wstring::npos);

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
	}

	if (inTrack && currentTrack.trackNumber > 0) {
		tracks.push_back(currentTrack);
	}

	file.close();

	for (size_t i = 0; i < tracks.size(); i++) {
		if (i + 1 < tracks.size()) {
			tracks[i].endLBA = tracks[i + 1].hasPregap
				? tracks[i + 1].pregapLBA - 1
				: tracks[i + 1].startLBA - 1;
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
		if (t.hasPregap) {
			std::cout << " (pregap at " << t.pregapLBA
				<< ", " << (t.startLBA - t.pregapLBA) << " frames)";
		}
		if (!t.title.empty()) {
			std::cout << " \"" << t.title << "\"";
		}
		std::cout << "\n";
	}

	return !tracks.empty();
}

// ============================================================================
// Helper: Convert LBA to MSF absolute disc time
// ============================================================================
static void LBAtoMSF(int lba, BYTE& m, BYTE& s, BYTE& f) {
	int absFrame = lba + 150;
	m = static_cast<BYTE>(absFrame / (75 * 60));
	s = static_cast<BYTE>((absFrame / 75) % 60);
	f = static_cast<BYTE>(absFrame % 75);
}

// ============================================================================
// Helper: Set Write Parameters Mode Page 0x05
// ============================================================================
static bool SetWriteParametersPage(ScsiDrive& drive, int subchannelMode) {
	BYTE modeData[60] = { 0 };

	BYTE* page = modeData + 8;
	page[0] = 0x05;
	page[1] = 0x32;

	BYTE expectedWriteType;
	BYTE expectedBlockType;

	switch (subchannelMode) {
	case 2:
		page[2] = 0x43;
		page[4] = 0x03;
		expectedWriteType = 0x03;
		expectedBlockType = 0x03;
		break;
	case 1:
		page[2] = 0x43;
		page[4] = 0x02;
		expectedWriteType = 0x03;
		expectedBlockType = 0x02;
		break;
	case 4:
		page[2] = 0x42;
		page[4] = 0x03;
		expectedWriteType = 0x02;
		expectedBlockType = 0x03;
		break;
	case 3:
		page[2] = 0x42;
		page[4] = 0x02;
		expectedWriteType = 0x02;
		expectedBlockType = 0x02;
		break;
	default:
		page[2] = 0x42;
		page[4] = 0x00;
		expectedWriteType = 0x02;
		expectedBlockType = 0x00;
		break;
	}

	page[3] = 0x00;
	page[5] = 0x00;
	page[8] = 0x00;

	WORD totalLen = 60;
	BYTE selectCdb[10] = { 0x55, 0x10, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };
	selectCdb[7] = static_cast<BYTE>((totalLen >> 8) & 0xFF);
	selectCdb[8] = static_cast<BYTE>(totalLen & 0xFF);

	BYTE senseKey = 0, asc = 0, ascq = 0;
	if (!drive.SendSCSIWithSense(selectCdb, sizeof(selectCdb), modeData, totalLen,
		&senseKey, &asc, &ascq, false)) {
		Console::Error("MODE SELECT for Write Parameters failed (");
		std::cout << drive.GetSenseDescription(senseKey, asc, ascq) << ")\n";
		return false;
	}

	BYTE senseCdb[10] = { 0x5A, 0x00, 0x05, 0x00, 0x00, 0x00, 0x00, 0x00, 0x3C, 0x00 };
	BYTE verifyBuf[60] = { 0 };
	if (drive.SendSCSI(senseCdb, sizeof(senseCdb), verifyBuf, sizeof(verifyBuf), true)) {
		WORD bdLen = (static_cast<WORD>(verifyBuf[6]) << 8) | verifyBuf[7];
		BYTE* vPage = verifyBuf + 8 + bdLen;
		BYTE writeType = vPage[2] & 0x0F;
		BYTE blockType = vPage[4];
		const char* modeName = (writeType == 0x03) ? "Raw DAO" : "SAO";
		const char* subName = (blockType == 0x03) ? "raw P-W" :
			(blockType == 0x02) ? "packed P-W" : "none";
		int blockSize = (blockType >= 0x02) ? 2448 : (blockType == 0x01) ? 2368 : 2352;
		Console::Success("Write parameters verified (");
		std::cout << modeName << ", Audio, " << blockSize << "-byte blocks, sub: " << subName << ")\n";

		if (writeType != expectedWriteType || blockType != expectedBlockType) {
			Console::Warning("Drive silently changed write parameters (requested write type 0x");
			std::cout << std::hex << std::setfill('0') << std::setw(2)
				<< static_cast<int>(expectedWriteType) << "/block 0x"
				<< std::setw(2) << static_cast<int>(expectedBlockType)
				<< ", got 0x" << std::setw(2) << static_cast<int>(writeType)
				<< "/0x" << std::setw(2) << static_cast<int>(blockType)
				<< std::dec << std::setfill(' ') << ")\n";
			return false;
		}
	}

	return true;
}

// ============================================================================
// Helper: Build and send SCSI CUE sheet
// ============================================================================
bool WriteDiscInternal::BuildAndSendCueSheet(ScsiDrive& drive,
	const std::vector<AudioCDCopier::TrackWriteInfo>& tracks,
	DWORD totalSectors, int subchannelMode) {

	BYTE trackDataForm;
	switch (subchannelMode) {
	case 1:
	case 3:
		trackDataForm = 0x02; break;
	case 2:
	case 4:
		trackDataForm = 0x03; break;
	default: trackDataForm = 0x00; break;
	}

	size_t entryCount = 2;
	entryCount++;
	for (size_t i = 0; i < tracks.size(); i++) {
		entryCount++;
		if (i > 0 && tracks[i].hasPregap && tracks[i].pregapLBA < tracks[i].startLBA) {
			entryCount++;
		}
	}

	size_t cueSheetSize = entryCount * 8;
	std::vector<BYTE> cueSheet(cueSheetSize, 0);
	size_t ei = 0;

	BYTE leadInDataForm;
	if (subchannelMode == 1 || subchannelMode == 2) {
		leadInDataForm = trackDataForm;
	}
	else {
		leadInDataForm = 0x01;
	}

	cueSheet[ei * 8 + 0] = 0x01;
	cueSheet[ei * 8 + 1] = 0x00;
	cueSheet[ei * 8 + 2] = 0x00;
	cueSheet[ei * 8 + 3] = leadInDataForm;
	ei++;

	cueSheet[ei * 8 + 0] = 0x01;
	cueSheet[ei * 8 + 1] = 0x01;
	cueSheet[ei * 8 + 2] = 0x00;
	cueSheet[ei * 8 + 3] = trackDataForm;
	cueSheet[ei * 8 + 5] = 0x00;
	cueSheet[ei * 8 + 6] = 0x00;
	cueSheet[ei * 8 + 7] = 0x00;
	ei++;

	for (size_t i = 0; i < tracks.size(); i++) {
		const auto& t = tracks[i];

		if (i > 0 && t.hasPregap && t.pregapLBA < t.startLBA) {
			BYTE m, s, f;
			LBAtoMSF(static_cast<int>(t.pregapLBA), m, s, f);

			cueSheet[ei * 8 + 0] = 0x01;
			cueSheet[ei * 8 + 1] = static_cast<BYTE>(t.trackNumber);
			cueSheet[ei * 8 + 2] = 0x00;
			cueSheet[ei * 8 + 3] = trackDataForm;
			cueSheet[ei * 8 + 5] = m;
			cueSheet[ei * 8 + 6] = s;
			cueSheet[ei * 8 + 7] = f;
			ei++;
		}

		BYTE m, s, f;
		LBAtoMSF(static_cast<int>(t.startLBA), m, s, f);

		cueSheet[ei * 8 + 0] = 0x01;
		cueSheet[ei * 8 + 1] = static_cast<BYTE>(t.trackNumber);
		cueSheet[ei * 8 + 2] = 0x01;
		cueSheet[ei * 8 + 3] = trackDataForm;
		cueSheet[ei * 8 + 5] = m;
		cueSheet[ei * 8 + 6] = s;
		cueSheet[ei * 8 + 7] = f;
		ei++;
	}

	{
		BYTE m, s, f;
		LBAtoMSF(static_cast<int>(totalSectors), m, s, f);
		cueSheet[ei * 8 + 0] = 0x01;
		cueSheet[ei * 8 + 1] = 0xAA;
		cueSheet[ei * 8 + 2] = 0x01;
		cueSheet[ei * 8 + 3] = trackDataForm;
		cueSheet[ei * 8 + 5] = m;
		cueSheet[ei * 8 + 6] = s;
		cueSheet[ei * 8 + 7] = f;
	}

	Console::Info("SCSI CUE sheet layout:\n");
	for (size_t i = 0; i < entryCount; i++) {
		BYTE* e = &cueSheet[i * 8];
		if (e[1] == 0x00)       std::cout << "  Lead-in ";
		else if (e[1] == 0xAA)  std::cout << "  Lead-out";
		else                    std::cout << "  Track " << std::setw(2) << static_cast<int>(e[1]);
		std::cout << "  Index " << static_cast<int>(e[2])
			<< "  DataForm 0x" << std::hex << std::setfill('0') << std::setw(2)
			<< static_cast<int>(e[3]) << std::dec
			<< "  MSF " << std::setfill('0') << std::setw(2) << static_cast<int>(e[5])
			<< ":" << std::setw(2) << static_cast<int>(e[6])
			<< ":" << std::setw(2) << static_cast<int>(e[7])
			<< std::setfill(' ') << "\n";
	}

	BYTE cdb[10] = { 0x5D, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };
	DWORD size = static_cast<DWORD>(cueSheetSize);
	cdb[6] = static_cast<BYTE>((size >> 16) & 0xFF);
	cdb[7] = static_cast<BYTE>((size >> 8) & 0xFF);
	cdb[8] = static_cast<BYTE>(size & 0xFF);

	BYTE senseKey = 0, asc = 0, ascq = 0;
	if (!drive.SendSCSIWithSense(cdb, sizeof(cdb), cueSheet.data(), size,
		&senseKey, &asc, &ascq, false)) {
		Console::Error("SEND CUE SHEET failed (");
		std::cout << drive.GetSenseDescription(senseKey, asc, ascq) << ")\n";
		return false;
	}

	Console::Success("CUE sheet accepted (");
	std::cout << entryCount << " entries, " << tracks.size() << " tracks)\n";
	return true;
}

// ============================================================================
// Helper: Prepare drive for writing
// ============================================================================
bool WriteDiscInternal::PrepareDriveForWrite(ScsiDrive& drive, int subchannelMode) {
	Console::Info("Checking drive readiness...\n");
	if (!WaitForDriveReady(drive, 15)) {
		Console::Error("Drive did not become ready\n");
		return false;
	}
	Console::Success("Drive is ready\n");

	Console::Info("Configuring write parameters...\n");
	if (!SetWriteParametersPage(drive, subchannelMode)) {
		Console::Error("Failed to configure write parameters\n");
		return false;
	}

	return true;
}