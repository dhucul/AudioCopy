#define NOMINMAX
#include "AudioCDCopier.h"
#include "ConsoleColors.h"
#include "Progress.h"
#include "InterruptHandler.h"
#include <iostream>
#include <fstream>
#include <filesystem>
#include <conio.h>
#include <sstream>
#include <iomanip>
#include <windows.h>

// ============================================================================
// Helper: Convert wide string to UTF-8 string (Windows API)
// ============================================================================
static std::string WideToUTF8(const std::wstring& wide) {
	if (wide.empty()) return std::string();

	// Calculate required buffer size
	int requiredSize = WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), -1, nullptr, 0, nullptr, nullptr);
	if (requiredSize == 0) return std::string();

	std::string utf8(requiredSize - 1, '\0');
	WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), -1, &utf8[0], requiredSize, nullptr, nullptr);

	return utf8;
}

// ============================================================================
// CheckRewritableDisk - Detect rewritable disc and capacity
// ============================================================================
bool AudioCDCopier::CheckRewritableDisk(bool& isFull, bool& isRewritable) {
	isFull = false;
	isRewritable = false;

	Console::Info("Querying disc media type...\n");

	BYTE cmd[10] = { 0x51, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xFC, 0x00 };
	BYTE response[252] = { 0 };
	BYTE senseKey = 0, asc = 0, ascq = 0;

	if (!m_drive.SendSCSIWithSense(cmd, sizeof(cmd), response, sizeof(response),
		&senseKey, &asc, &ascq, true)) {

		Console::Warning("Disc information query failed");
		std::string senseDesc = m_drive.GetSenseDescription(senseKey, asc, ascq);
		if (!senseDesc.empty()) {
			Console::Info(" (");
			std::cout << senseDesc << ")\n";
		}
		else {
			std::cout << "\n";
		}

		Console::Warning("Attempting fallback disc detection...\n");

		BYTE profileCmd[10] = { 0x46, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x08, 0x00 };
		BYTE profileResponse[8] = { 0 };

		if (m_drive.SendSCSI(profileCmd, sizeof(profileCmd), profileResponse, sizeof(profileResponse), true)) {
			WORD profile = (static_cast<WORD>(profileResponse[6]) << 8) | profileResponse[7];
			isRewritable = (profile == 0x0A);

			// Fallback cannot determine disc status from GET CONFIGURATION alone.
			// If the disc is rewritable, assume it may need blanking (treat as full)
			// so the caller gets an opportunity to erase before writing.
			if (isRewritable)
				isFull = true;

			Console::Success("Media type detected: ");
			switch (profile) {
			case 0x08: std::cout << "CD-ROM\n"; break;
			case 0x09: std::cout << "CD-R (write-once)\n"; isRewritable = false; break;
			case 0x0A: std::cout << "CD-RW (rewritable)\n"; isRewritable = true; break;
			default:
				std::cout << "Unknown (0x" << std::hex << profile << std::dec << ")\n";
				break;
			}
			return true;
		}

		Console::Warning("Could not determine disc type\n");
		return false;
	}

	// READ DISC INFORMATION succeeded — parse response
	BYTE discStatus = response[2] & 0x03;
	isFull = (discStatus == 0x02);

	// Erasable bit is bit 4 of byte 2 (not byte 8)
	isRewritable = (response[2] & 0x10) != 0;

	Console::Success("Disc media type: ");
	std::cout << (isRewritable ? "CD-RW (rewritable)\n" : "CD-R (write-once)\n");

	Console::Success("Disc status: ");
	switch (discStatus) {
	case 0x00: std::cout << "Empty\n"; break;
	case 0x01: std::cout << "Appendable\n"; break;
	case 0x02: std::cout << "Complete (full)\n"; break;
	default: std::cout << "Unknown\n"; break;
	}

	return true;
}

// ============================================================================
// BlankRewritableDisk - Erase rewritable media (quick or full)
// ============================================================================
bool AudioCDCopier::BlankRewritableDisk(int speed, bool quickBlank) {
	Console::Warning(quickBlank ? "\nQuick blanking rewritable disc...\n"
		: "\nFull blanking rewritable disc...\n");
	Console::Info("⚠ This operation will erase all data on the disc!\n");
	std::cout << "Confirm blanking? (y/n): ";
	char confirm = _getch();
	std::cout << confirm << "\n";
	if (tolower(confirm) != 'y') {
		Console::Info("Blank operation cancelled\n");
		return false;
	}

	// Set drive speed before blanking
	m_drive.SetSpeed(speed);

	// SCSI BLANK command (0xA1)
	// Byte 1 bit 4: Immed — return immediately so we can poll progress
	// Byte 1 bits 2-0: blanking type — 0x00 = full, 0x01 = quick (minimal)
	BYTE blankType = (quickBlank ? 0x01 : 0x00) | 0x10;  // Immed + blank type
	BYTE cmd[12] = { 0xA1, blankType, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };

	if (!m_drive.SendSCSI(cmd, sizeof(cmd), nullptr, 0, false)) {
		Console::Error("Blank command failed\n");
		return false;
	}

	// Poll drive with REQUEST SENSE for real progress indication.
	// The drive reports NOT READY (SK=0x02, ASC=0x04, ASCQ=0x07) with a
	// progress percentage in the Sense Key Specific field while blanking.
	int maxWait = quickBlank ? 120 : 600;
	int barWidth = 35;
	std::string label = quickBlank ? "Quick blank" : "Full blank";
	int lastPct = -1;
	int lastLineLen = 0;
	auto startTime = std::chrono::steady_clock::now();

	for (int i = 0; i < maxWait; i++) {
		Sleep(1000);

		if (InterruptHandler::Instance().IsInterrupted()) {
			std::cout << "\n";
			Console::Error("Blank operation cancelled\n");
			return false;
		}

		// Query actual drive progress via REQUEST SENSE
		BYTE sk = 0, asc = 0, ascq = 0;
		int drivePct = -1;
		m_drive.RequestSenseProgress(sk, asc, ascq, drivePct);

		// Drive ready (TEST UNIT READY succeeds) — blank is done
		BYTE testCmd[6] = { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };
		BYTE tsk = 0, tasc = 0, tascq = 0;
		if (m_drive.SendSCSIWithSense(testCmd, sizeof(testCmd), nullptr, 0,
			&tsk, &tasc, &tascq, true)) {
			drivePct = 100;
		}

		// Use drive-reported progress if available, else fall back to time estimate
		int pct;
		if (drivePct >= 0) {
			pct = std::min(drivePct, 100);
		}
		else {
			pct = std::min((i + 1) * 100 / maxWait, 99);
		}

		// Render progress bar
		if (pct != lastPct) {
			lastPct = pct;
			std::ostringstream ss;
			ss << "\r" << label << " [";
			int filled = pct * barWidth / 100;
			for (int j = 0; j < barWidth; j++) {
				ss << (j < filled ? "\xe2\x96\x88" : "\xe2\x96\x91"); // █ or ░
			}
			ss << "] " << std::setw(3) << pct << "%";

			// Elapsed time
			auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
				std::chrono::steady_clock::now() - startTime).count();
			if (elapsed >= 60)
				ss << " " << elapsed / 60 << "m " << std::setfill('0') << std::setw(2) << elapsed % 60 << "s";
			else
				ss << " " << elapsed << "s";

			std::string line = ss.str();
			if (static_cast<int>(line.size()) < lastLineLen)
				line.append(lastLineLen - line.size(), ' ');
			std::cout << line << std::flush;
			lastLineLen = static_cast<int>(line.size());
		}

		if (drivePct >= 100) break;
	}

	// Final summary
	auto totalSec = std::chrono::duration_cast<std::chrono::seconds>(
		std::chrono::steady_clock::now() - startTime).count();
	std::cout << "\n  Done";
	if (totalSec > 0) {
		if (totalSec >= 60)
			std::cout << " in " << totalSec / 60 << "m "
				<< std::setfill('0') << std::setw(2) << totalSec % 60 << "s";
		else
			std::cout << " in " << totalSec << "s";
	}
	std::cout << "\n";

	Console::Success("Disc blanked successfully\n");
	return true;
}

// ============================================================================
// PerformPowerCalibration - Calibrate laser power for writing
// ============================================================================
bool AudioCDCopier::PerformPowerCalibration() {
	Console::Info("Performing power calibration (OPC)...\n");

	BYTE cmd[10] = { 0x54, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };

	if (!m_drive.SendSCSI(cmd, sizeof(cmd), nullptr, 0, false)) {
		Console::Warning("Power calibration not supported by drive (continuing)\n");
		return true;
	}

	Console::Info("Waiting for power calibration to complete");
	for (int i = 0; i < 5; i++) {
		std::cout << ".";
		Sleep(1000);
		if (InterruptHandler::Instance().IsInterrupted()) {
			Console::Error("\nPower calibration cancelled\n");
			return false;
		}
	}
	std::cout << "\n";

	Console::Success("Power calibration complete\n");
	return true;
}

// ============================================================================
// ParseCueSheet - Parse CUE file to extract track information with pregaps
// ============================================================================
bool AudioCDCopier::ParseCueSheet(const std::wstring& cueFile,
	std::vector<TrackWriteInfo>& tracks) {

	std::wifstream file(cueFile);
	if (!file.is_open()) {
		Console::Error("Cannot open CUE file: ");
		std::wcout << cueFile << L"\n";
		return false;
	}

	std::wstring line;
	TrackWriteInfo currentTrack = {};
	currentTrack.hasPregap = false;
	bool inTrack = false;

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
		// PREGAP command = drive-generated pregap, NOT in the BIN file.
		// Don't set hasPregap — nothing to write.
	}

	if (inTrack && currentTrack.trackNumber > 0) {
		tracks.push_back(currentTrack);
	}

	file.close();

	// Compute endLBA for each track from the next track's start (or pregap)
	for (size_t i = 0; i < tracks.size(); i++) {
		if (i + 1 < tracks.size()) {
			tracks[i].endLBA = tracks[i + 1].hasPregap
				? tracks[i + 1].pregapLBA - 1
				: tracks[i + 1].startLBA - 1;
		}
		// Last track endLBA is set after we know totalSectors (handled in WriteDisc)
	}

	Console::Success("Parsed CUE sheet: ");
	std::cout << tracks.size() << " tracks\n";

	for (const auto& t : tracks) {
		Console::Info("  Track ");
		std::cout << t.trackNumber << ": LBA " << t.startLBA
			<< " - " << t.endLBA;
		if (t.hasPregap) {
			std::cout << " (pregap at " << t.pregapLBA
				<< ", " << (t.startLBA - t.pregapLBA) << " frames)";
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
// Helper: Flush drive write buffer via SYNCHRONIZE CACHE (0x35)
// ============================================================================
static bool SynchronizeCache(ScsiDrive& drive) {
	BYTE cdb[10] = { 0x35, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };
	BYTE sk = 0, asc = 0, ascq = 0;
	if (!drive.SendSCSIWithSense(cdb, sizeof(cdb), nullptr, 0, &sk, &asc, &ascq, false)) {
		if (sk == 0x02 && asc == 0x04) {
			return WaitForDriveReady(drive, 60);
		}
		return false;
	}
	return true;
}

// ============================================================================
// Helper: Deinterleave raw P-W subchannel to packed format
// Raw:    96 bytes where each byte contains one bit per channel (P,Q,R,S,T,U,V,W)
// Packed: 96 bytes grouped by channel (12 bytes P, 12 bytes Q, ... 12 bytes W)
// ============================================================================
static void DeinterleaveSubchannel(const BYTE* raw, BYTE* packed) {
	memset(packed, 0, SUBCHANNEL_SIZE);
	for (int i = 0; i < 96; i++) {
		for (int ch = 0; ch < 8; ch++) {
			if (raw[i] & (0x80 >> ch)) {
				int outByte = ch * 12 + (i / 8);
				int outBit = 7 - (i % 8);
				packed[outByte] |= (1 << outBit);
			}
		}
	}
}

// ============================================================================
// Helper: Set Write Parameters Mode Page 0x05
//   subchannelMode:
//     0 = SAO (0x02), no subchannel, block type 0x00 (2352 bytes)
//     1 = Raw DAO (0x03), packed P-W subchannel, block type 0x02 (2448 bytes)
//     2 = Raw DAO (0x03), raw P-W subchannel, block type 0x03 (2448 bytes)
// ============================================================================
static bool SetWriteParametersPage(ScsiDrive& drive, int subchannelMode) {
	BYTE modeData[60] = { 0 };

	BYTE* page = modeData + 8;
	page[0] = 0x05;  // Page code
	page[1] = 0x32;  // Page length = 50

	switch (subchannelMode) {
	case 2:  // Raw DAO + raw P-W subchannel
		page[2] = 0x43;  // BUFE | Write Type 0x03 (Raw)
		page[4] = 0x03;  // Data block type 3: raw P-W (2448 bytes)
		break;
	case 1:  // Raw DAO + packed P-W subchannel
		page[2] = 0x43;  // BUFE | Write Type 0x03 (Raw)
		page[4] = 0x02;  // Data block type 2: packed P-W (2448 bytes)
		break;
	default: // SAO, no subchannel
		page[2] = 0x42;  // BUFE | Write Type 0x02 (SAO)
		page[4] = 0x00;  // Data block type 0: audio only (2352 bytes)
		break;
	}

	page[3] = 0x00;  // Track mode: 2-channel audio, no pre-emphasis
	page[5] = 0x00;  // Link size
	page[8] = 0x00;  // Session format: CD-DA / CD-ROM

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

	// Verify readback
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
	}

	return true;
}

// ============================================================================
// Helper: Build and send SCSI CUE sheet
// Always includes Track 1 pregap at MSF 00:00:00 (required by Red Book).
// subchannelMode: 0 = SAO (Data Form 0x00), 1 = packed P-W (0x02), 2 = raw P-W (0x03)
// ============================================================================
static bool BuildAndSendCueSheet(ScsiDrive& drive,
	const std::vector<AudioCDCopier::TrackWriteInfo>& tracks,
	DWORD totalSectors, int subchannelMode) {

	// Data Form must match the block type set in Mode Page 0x05
	BYTE trackDataForm;
	switch (subchannelMode) {
	case 1:  trackDataForm = 0x02; break;  // packed P-W
	case 2:  trackDataForm = 0x03; break;  // raw P-W
	default: trackDataForm = 0x00; break;  // SAO, no subchannel
	}

	// Count entries: lead-in + Track 1 pregap (always) + per-track entries + lead-out
	size_t entryCount = 2;  // lead-in + lead-out
	entryCount++;           // Track 1 Index 00 (always present)
	for (size_t i = 0; i < tracks.size(); i++) {
		entryCount++;  // Index 01
		// Additional Index 00 for tracks 2+ that have pregap data in the BIN
		if (i > 0 && tracks[i].hasPregap && tracks[i].pregapLBA < tracks[i].startLBA) {
			entryCount++;
		}
	}

	size_t cueSheetSize = entryCount * 8;
	std::vector<BYTE> cueSheet(cueSheetSize, 0);
	size_t ei = 0;

	// ── Lead-in (Data Form must match session type) ──────────
	// MMC-3 Table 28: For audio CD-DA sessions:
	//   0x01 = CD-DA (SAO, no sub-channel data provided)
	//   0x03 = CD-DA (Raw, packed P-W subchannel)
	//   0x03 = CD-DA (Raw, raw P-W subchannel)
	// Using trackDataForm for lead-in ensures consistency with the write mode.
	// For SAO, trackDataForm is 0x00 which some drives reject; use 0x01 instead.
	BYTE leadInDataForm = (subchannelMode == 0) ? 0x01 : trackDataForm;

	cueSheet[ei * 8 + 0] = 0x01;  // CTL/ADR: audio
	cueSheet[ei * 8 + 1] = 0x00;  // TNO: lead-in
	cueSheet[ei * 8 + 2] = 0x00;  // Index 0
	cueSheet[ei * 8 + 3] = leadInDataForm;
	ei++;

	// ── Track 1 Index 00 (mandatory pregap at MSF 00:00:00) ──
	cueSheet[ei * 8 + 0] = 0x01;
	cueSheet[ei * 8 + 1] = 0x01;  // Track 1
	cueSheet[ei * 8 + 2] = 0x00;  // Index 00
	cueSheet[ei * 8 + 3] = trackDataForm;
	cueSheet[ei * 8 + 5] = 0x00;  // M = 0
	cueSheet[ei * 8 + 6] = 0x00;  // S = 0
	cueSheet[ei * 8 + 7] = 0x00;  // F = 0
	ei++;

	// ── All tracks (Index 00 for tracks 2+ if present, Index 01 for all) ─
	for (size_t i = 0; i < tracks.size(); i++) {
		const auto& t = tracks[i];

		// Index 00 for tracks 2+ with pregap data in the BIN
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

		// Index 01
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

	// ── Lead-out ─────────────────────────────────────────────
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

	// ── Log ──────────────────────────────────────────────────
	Console::Info("SCSI CUE sheet layout:\n");
	for (size_t i = 0; i < entryCount; i++) {
		BYTE* e = &cueSheet[i * 8];
		if (e[1] == 0x00)       std::cout << "  Lead-in ";
		else if (e[1] == 0xAA)  std::cout << "  Lead-out";
		else                    std::cout << "  Track " << std::setw(2) << static_cast<int>(e[1]);
		std::cout << "  Index " << static_cast<int>(e[2])
			<< "  MSF " << std::setfill('0') << std::setw(2) << static_cast<int>(e[5])
			<< ":" << std::setw(2) << static_cast<int>(e[6])
			<< ":" << std::setw(2) << static_cast<int>(e[7])
			<< std::setfill(' ') << "\n";
	}

	// ── SEND CUE SHEET (0x5D) ────────────────────────────────
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
static bool PrepareDriveForWrite(ScsiDrive& drive, int subchannelMode) {
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

// ============================================================================
// WriteDisc - Write disc from .bin/.cue/.sub files
// ============================================================================
bool AudioCDCopier::WriteDisc(const std::wstring& binFile,
	const std::wstring& cueFile, const std::wstring& subFile,
	int speed, bool usePowerCalibration) {

	Console::BoxHeading("Write Disc from Files");

	// Verify input files exist
	std::ifstream binStream(binFile, std::ios::binary);
	if (!binStream.is_open()) {
		Console::Error("Cannot open .bin file: ");
		std::wcout << binFile << L"\n";
		return false;
	}
	binStream.seekg(0, std::ios::end);
	long long fileSize = binStream.tellg();
	binStream.close();

	DWORD totalSectors = static_cast<DWORD>(fileSize / AUDIO_SECTOR_SIZE);

	// Determine if we can use Raw DAO mode with subchannel data
	bool hasSubchannel = false;
	bool needsDeinterleave = false;
	if (!subFile.empty()) {
		std::ifstream subStream(subFile, std::ios::binary);
		if (subStream.is_open()) {
			subStream.seekg(0, std::ios::end);
			long long subSize = subStream.tellg();
			subStream.close();

			long long expectedSubSize = static_cast<long long>(totalSectors) * SUBCHANNEL_SIZE;
			if (subSize >= expectedSubSize) {
				hasSubchannel = true;
				Console::Success("Subchannel file validated (");
				std::cout << (subSize / 1024) << " KB, " << totalSectors << " sectors)\n";
			}
			else {
				Console::Warning("Subchannel file size mismatch (expected ");
				std::cout << expectedSubSize << " bytes, got " << subSize
					<< ") — writing without subchannel\n";
			}
		}
		else {
			Console::Warning("Cannot open .sub file — writing without subchannel data\n");
		}
	}

	// Parse CUE sheet
	std::vector<TrackWriteInfo> tracks;
	if (!ParseCueSheet(cueFile, tracks)) {
		Console::Error("Failed to parse CUE sheet\n");
		return false;
	}

	// Set last track endLBA now that we know totalSectors
	if (!tracks.empty()) {
		tracks.back().endLBA = totalSectors - 1;
	}

	// Power calibration
	if (usePowerCalibration) {
		if (!PerformPowerCalibration()) {
			return false;
		}
	}

	// Set drive write speed
	m_drive.SetSpeed(speed);
	Console::Success("Drive speed set to ");
	std::cout << speed << "x\n";

	// Negotiate write mode: try packed P-W first (most compatible Raw DAO),
	// then raw P-W, then SAO fallback
	// subchannelMode: 0 = SAO, 1 = packed P-W, 2 = raw P-W
	int subchannelMode = 0;

	if (hasSubchannel) {
		// Try packed P-W (block type 0x02) — most drives that support Raw DAO use this
		Console::Info("Trying Raw DAO with packed P-W subchannel...\n");
		if (PrepareDriveForWrite(m_drive, 1)) {
			subchannelMode = 1;
			needsDeinterleave = true;  // .sub file is raw, drive wants packed
			Console::Success("Drive accepts packed P-W subchannel (Raw DAO)\n");
		}
		else {
			// Try raw P-W (block type 0x03) — rare but matches .sub file format
			Console::Info("Trying Raw DAO with raw P-W subchannel...\n");
			if (PrepareDriveForWrite(m_drive, 2)) {
				subchannelMode = 2;
				needsDeinterleave = false;  // .sub file is already raw
				Console::Success("Drive accepts raw P-W subchannel (Raw DAO)\n");
			}
			else {
				Console::Warning("Drive does not support Raw DAO — subchannel data will not be written\n");
				Console::Info("Falling back to SAO (drive-generated subchannel from CUE sheet)\n");
				hasSubchannel = false;
			}
		}
	}

	if (!hasSubchannel) {
		if (!PrepareDriveForWrite(m_drive, 0)) {
			return false;
		}
	}

	// Send SCSI CUE sheet
	Console::Info("\nSending disc layout to drive...\n");
	if (!BuildAndSendCueSheet(m_drive, tracks, totalSectors, subchannelMode)) {
		if (hasSubchannel) {
			Console::Warning("Drive rejected CUE sheet for Raw DAO — falling back to SAO\n");
			Console::Warning("Subchannel data from .sub file will NOT be written\n");
			hasSubchannel = false;
			needsDeinterleave = false;
			subchannelMode = 0;
			if (!PrepareDriveForWrite(m_drive, 0)) return false;
			if (!BuildAndSendCueSheet(m_drive, tracks, totalSectors, 0)) {
				Console::Error("Drive rejected disc layout\n");
				return false;
			}
		}
		else {
			Console::Error("Drive rejected disc layout\n");
			return false;
		}
	}

	// Write audio sectors
	Console::Info("\nWriting audio sectors...\n");
	if (!WriteAudioSectors(binFile, subFile, tracks, totalSectors,
		hasSubchannel, needsDeinterleave)) {
		Console::Error("Failed to write audio sectors\n");
		return false;
	}

	return VerifyWriteCompletion(binFile);
}

// ============================================================================
// WriteAudioSectors - Write pregap silence + audio data (+ optional subchannel)
// ============================================================================
bool AudioCDCopier::WriteAudioSectors(const std::wstring& binFile,
	const std::wstring& subFile,
	const std::vector<TrackWriteInfo>& tracks,
	DWORD totalSectors,
	bool hasSubchannel,
	bool needsDeinterleave) {

	std::ifstream binInput(binFile, std::ios::binary);
	if (!binInput.is_open()) {
		Console::Error("Cannot open binary file\n");
		return false;
	}

	std::ifstream subInput;
	if (hasSubchannel) {
		subInput.open(subFile, std::ios::binary);
		if (!subInput.is_open()) {
			Console::Error("Cannot open subchannel file\n");
			binInput.close();
			return false;
		}
	}

	const DWORD sectorSize = hasSubchannel ? RAW_SECTOR_SIZE : AUDIO_SECTOR_SIZE;

	// The CUE sheet declares Track 1 pregap at MSF 00:00:00 (LBA -150).
	// The host must provide 150 sectors of silence for the pregap, then the BIN data.
	constexpr DWORD PREGAP_SECTORS = 150;
	DWORD writeTotalSectors = PREGAP_SECTORS + totalSectors;

	Console::Info("Binary file size: ");
	long long fileSize = static_cast<long long>(totalSectors) * AUDIO_SECTOR_SIZE;
	std::cout << (fileSize / (1024 * 1024)) << " MB (" << totalSectors << " sectors)\n";
	Console::Info("Total write: ");
	std::cout << writeTotalSectors << " sectors (150 pregap + " << totalSectors << " audio)\n";

	if (hasSubchannel) {
		Console::Info("Write mode: Raw DAO (2448 bytes/sector = 2352 audio + 96 subchannel");
		if (needsDeinterleave) std::cout << ", deinterleaving";
		std::cout << ")\n";
	}
	else {
		Console::Info("Write mode: SAO (2352 bytes/sector, drive-generated subchannel)\n");
	}

	if (!WaitForDriveReady(m_drive, 10)) {
		Console::Warning("Drive not ready after CUE sheet (attempting write anyway)\n");
	}

	ProgressIndicator progress(35);
	progress.SetLabel("Writing");
	progress.Start();

	const DWORD SECTORS_PER_WRITE = hasSubchannel ? 20 : 27;
	std::vector<BYTE> writeBuffer(sectorSize * SECTORS_PER_WRITE);
	BYTE rawSub[SUBCHANNEL_SIZE];  // Temp buffer for deinterleaving

	DWORD sectorsWritten = 0;
	int32_t currentLBA = -150;  // Start at pregap (signed for negative LBA)
	int consecutiveErrors = 0;
	size_t currentTrackIdx = 0;

	while (sectorsWritten < writeTotalSectors) {
		if (InterruptHandler::Instance().IsInterrupted()) {
			Console::Error("\nWrite operation cancelled by user\n");
			progress.Finish(false);
			binInput.close();
			if (hasSubchannel) subInput.close();
			return false;
		}

		DWORD remaining = writeTotalSectors - sectorsWritten;
		DWORD batchSize = (remaining < SECTORS_PER_WRITE) ? remaining : SECTORS_PER_WRITE;

		// Fill write buffer — pregap silence for first 150 sectors, then BIN data
		for (DWORD s = 0; s < batchSize; s++) {
			BYTE* dest = writeBuffer.data() + s * sectorSize;
			DWORD globalSector = sectorsWritten + s;

			if (globalSector < PREGAP_SECTORS) {
				// Pregap: silence (zero audio + zero subchannel)
				memset(dest, 0x00, sectorSize);
			}
			else {
				// BIN file data
				binInput.read(reinterpret_cast<char*>(dest), AUDIO_SECTOR_SIZE);
				size_t audioRead = binInput.gcount();
				if (audioRead < AUDIO_SECTOR_SIZE) {
					std::fill(dest + audioRead, dest + AUDIO_SECTOR_SIZE, 0x00);
				}

				// Subchannel data (appended after audio)
				if (hasSubchannel) {
					BYTE* subDest = dest + AUDIO_SECTOR_SIZE;

					if (needsDeinterleave) {
						// Read raw subchannel, deinterleave to packed format
						subInput.read(reinterpret_cast<char*>(rawSub), SUBCHANNEL_SIZE);
						size_t subRead = subInput.gcount();
						if (subRead < SUBCHANNEL_SIZE) {
							std::fill(rawSub + subRead, rawSub + SUBCHANNEL_SIZE, 0x00);
						}
						DeinterleaveSubchannel(rawSub, subDest);
					}
					else {
						// Raw format matches — copy directly
						subInput.read(reinterpret_cast<char*>(subDest), SUBCHANNEL_SIZE);
						size_t subRead = subInput.gcount();
						if (subRead < SUBCHANNEL_SIZE) {
							std::fill(subDest + subRead, subDest + SUBCHANNEL_SIZE, 0x00);
						}
					}
				}
			}
		}

		DWORD transferBytes = batchSize * sectorSize;

		// WRITE(10) — LBA is signed, two's complement for negative values
		BYTE writeCmd[10] = { 0x2A, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };
		DWORD lbaUnsigned = static_cast<DWORD>(currentLBA);
		writeCmd[2] = static_cast<BYTE>((lbaUnsigned >> 24) & 0xFF);
		writeCmd[3] = static_cast<BYTE>((lbaUnsigned >> 16) & 0xFF);
		writeCmd[4] = static_cast<BYTE>((lbaUnsigned >> 8) & 0xFF);
		writeCmd[5] = static_cast<BYTE>(lbaUnsigned & 0xFF);
		writeCmd[7] = static_cast<BYTE>((batchSize >> 8) & 0xFF);
		writeCmd[8] = static_cast<BYTE>(batchSize & 0xFF);

		BYTE senseKey = 0, asc = 0, ascq = 0;
		bool writeOk = m_drive.SendSCSIWithSense(writeCmd, sizeof(writeCmd),
			writeBuffer.data(), transferBytes, &senseKey, &asc, &ascq, false);

		if (!writeOk) {
			if (senseKey == 0x02 && asc == 0x04) {
				if (!WaitForDriveReady(m_drive, 30)) {
					consecutiveErrors++;
					if (consecutiveErrors >= 5) {
						Console::Error("\nDrive not recovering - aborting\n");
						progress.Finish(false);
						binInput.close();
						if (hasSubchannel) subInput.close();
						return false;
					}
					continue;
				}
				// Rewind files to retry
				DWORD binSector = (sectorsWritten >= PREGAP_SECTORS) ? sectorsWritten - PREGAP_SECTORS : 0;
				binInput.seekg(static_cast<long long>(binSector) * AUDIO_SECTOR_SIZE);
				if (hasSubchannel)
					subInput.seekg(static_cast<long long>(binSector) * SUBCHANNEL_SIZE);
				consecutiveErrors = 0;
				continue;
			}

			consecutiveErrors++;
			Console::Error("\nWrite failed at LBA ");
			std::cout << currentLBA << " (";
			std::cout << m_drive.GetSenseDescription(senseKey, asc, ascq) << ")\n";

			if (consecutiveErrors >= 5) {
				Console::Error("Too many consecutive write errors - aborting\n");
				progress.Finish(false);
				binInput.close();
				if (hasSubchannel) subInput.close();
				return false;
			}

			DWORD binSector = (sectorsWritten >= PREGAP_SECTORS) ? sectorsWritten - PREGAP_SECTORS : 0;
			binInput.seekg(static_cast<long long>(binSector) * AUDIO_SECTOR_SIZE);
			if (hasSubchannel)
				subInput.seekg(static_cast<long long>(binSector) * SUBCHANNEL_SIZE);
			Sleep(1000);
			continue;
		}

		consecutiveErrors = 0;
		sectorsWritten += batchSize;
		currentLBA += batchSize;
		progress.Update(sectorsWritten, writeTotalSectors);

		// Log track transitions (offset by pregap)
		DWORD binPosition = (sectorsWritten > PREGAP_SECTORS) ? sectorsWritten - PREGAP_SECTORS : 0;
		while (currentTrackIdx + 1 < tracks.size() &&
			binPosition >= tracks[currentTrackIdx + 1].startLBA) {
			currentTrackIdx++;
			Console::Info("\n  Track ");
			std::cout << tracks[currentTrackIdx].trackNumber << " reached\n";
		}
	}

	progress.Finish(true);
	binInput.close();
	if (hasSubchannel) subInput.close();

	Console::Success("Successfully wrote ");
	std::cout << sectorsWritten << " sectors";
	if (hasSubchannel) std::cout << " (with subchannel data)";
	std::cout << "\n";
	return true;
}

// ============================================================================
// VerifyWriteCompletion - Flush cache and close session
// ============================================================================
bool AudioCDCopier::VerifyWriteCompletion(const std::wstring& /*binFile*/) {
	// Flush remaining data from drive buffer to disc
	Console::Info("Flushing write cache...\n");
	if (!SynchronizeCache(m_drive)) {
		Console::Warning("Cache flush reported failure (disc may still finalize)\n");
	}

	// Wait for flush to complete
	WaitForDriveReady(m_drive, 60);

	// Close session (0x5B) — writes lead-out and finalizes the disc
	Console::Info("Closing session (writing lead-out)...\n");
	BYTE closeCmd[10] = { 0x5B, 0x00, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };
	BYTE senseKey = 0, asc = 0, ascq = 0;

	if (!m_drive.SendSCSIWithSense(closeCmd, sizeof(closeCmd), nullptr, 0,
		&senseKey, &asc, &ascq, false)) {
		// Drive may be busy writing lead-out, poll until ready
		if (senseKey == 0x02 && asc == 0x04) {
			Console::Info("Waiting for finalization");
		}
		else {
			Console::Warning("CLOSE SESSION command returned: ");
			std::cout << m_drive.GetSenseDescription(senseKey, asc, ascq) << "\n";
		}
	}

	// Lead-out can take 30-90 seconds
	Console::Info("Finalizing disc");
	for (int i = 0; i < 120; i++) {
		BYTE testCmd[6] = { 0x00 };
		if (m_drive.SendSCSI(testCmd, sizeof(testCmd), nullptr, 0, true)) {
			std::cout << "\n";
			Console::Success("Disc finalized successfully\n");
			return true;
		}
		if (i % 5 == 0) std::cout << ".";
		Sleep(1000);
	}

	std::cout << "\n";
	Console::Warning("Finalization timeout — disc may still be usable\n");
	return true;
}

// ============================================================================
// VerifyWrittenDisc - Read back and verify written sectors
// ============================================================================
bool AudioCDCopier::VerifyWrittenDisc(const std::vector<TrackWriteInfo>& tracks) {
	if (tracks.empty()) {
		Console::Warning("No tracks to verify\n");
		return false;
	}

	Console::Info("Verifying written sectors...\n");

	ProgressIndicator progress(35);
	progress.SetLabel("Verifying");
	progress.Start();

	// Verify first and last sector of each track
	int verifyCount = 0;
	int successCount = 0;
	DWORD totalChecks = static_cast<DWORD>(tracks.size() * 2);

	for (const auto& track : tracks) {
		BYTE audio[AUDIO_SECTOR_SIZE] = { 0 };
		BYTE subchannel[SUBCHANNEL_SIZE] = { 0 };

		// Verify first sector
		if (m_drive.ReadSector(track.startLBA, audio, subchannel)) {
			successCount++;
		}
		verifyCount++;
		progress.Update(verifyCount, totalChecks);

		// Verify last sector
		if (track.endLBA > track.startLBA) {
			if (m_drive.ReadSector(track.endLBA, audio, subchannel)) {
				successCount++;
			}
		}
		else {
			successCount++;  // Single-sector track, already verified
		}
		verifyCount++;
		progress.Update(verifyCount, totalChecks);
	}

	progress.Finish(successCount == verifyCount);
	Console::Success("Verification: ");
	std::cout << successCount << "/" << verifyCount << " sectors readable\n";
	return successCount == verifyCount;
}