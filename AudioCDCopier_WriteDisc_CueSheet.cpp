#define NOMINMAX
#include "AudioCDCopier.h"
#include "ConsoleColors.h"
#include "WriteDiscInternal.h"
#include <iomanip>
#include <iostream>
#include <vector>
#include <windows.h>

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
static bool SetWriteParametersPage(ScsiDrive& drive, int subchannelMode, bool quiet = false) {
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
	case 5:  // Raw without subchannel
		page[2] = 0x43;
		page[4] = 0x00;
		expectedWriteType = 0x03;
		expectedBlockType = 0x00;
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
		if (!quiet) {
			Console::Error("MODE SELECT for Write Parameters failed (");
			std::cout << drive.GetSenseDescription(senseKey, asc, ascq) << ")\n";
		}
		return false;
	}

	BYTE senseCdb[10] = { 0x5A, 0x00, 0x05, 0x00, 0x00, 0x00, 0x00, 0x00, 0x3C, 0x00 };
	BYTE verifyBuf[60] = { 0 };
	if (drive.SendSCSI(senseCdb, sizeof(senseCdb), verifyBuf, sizeof(verifyBuf), true)) {
		WORD bdLen = (static_cast<WORD>(verifyBuf[6]) << 8) | verifyBuf[7];
		BYTE* vPage = verifyBuf + 8 + bdLen;
		BYTE writeType = vPage[2] & 0x0F;
		BYTE blockType = vPage[4];
		const char* modeName = (writeType == 0x03) ? "Raw" : "DAO";
		const char* subName = (blockType == 0x03) ? "raw P-W" :
			(blockType == 0x02) ? "packed P-W" : "none";
		int blockSize = (blockType >= 0x02) ? 2448 : (blockType == 0x01) ? 2368 : 2352;

		if (!quiet) {
			Console::Success("Write parameters verified (");
			std::cout << modeName << ", Audio, " << blockSize << "-byte blocks, sub: " << subName << ")\n";
		}

		if (writeType != expectedWriteType || blockType != expectedBlockType) {
			if (!quiet) {
				Console::Warning("Drive silently changed write parameters (requested write type 0x");
				std::cout << std::hex << std::setfill('0') << std::setw(2)
					<< static_cast<int>(expectedWriteType) << "/block 0x"
					<< std::setw(2) << static_cast<int>(expectedBlockType)
					<< ", got 0x" << std::setw(2) << static_cast<int>(writeType)
					<< "/0x" << std::setw(2) << static_cast<int>(blockType)
					<< std::dec << std::setfill(' ') << ")\n";
			}
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
	DWORD totalSectors, int subchannelMode, bool verbose) {

	if (tracks.empty()) {
		Console::Error("Cannot build CUE sheet: no tracks\n");
		return false;
	}

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

	// Lead-in data form must match track data form
	BYTE leadInDataForm = trackDataForm;

	cueSheet[ei * 8 + 0] = 0x01;
	cueSheet[ei * 8 + 1] = 0x00;
	cueSheet[ei * 8 + 2] = 0x00;
	cueSheet[ei * 8 + 3] = leadInDataForm;
	ei++;

	cueSheet[ei * 8 + 0] = 0x01;
	cueSheet[ei * 8 + 1] = static_cast<BYTE>(tracks[0].trackNumber);
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

	if (verbose) {
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
	}
	else {
		Console::Info("Sending CUE sheet (");
		std::cout << entryCount << " entries, DataForm 0x"
			<< std::hex << std::setfill('0') << std::setw(2)
			<< static_cast<int>(trackDataForm) << std::dec
			<< std::setfill(' ') << ")...\n";
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
bool WriteDiscInternal::PrepareDriveForWrite(ScsiDrive& drive, int subchannelMode, bool quiet) {
	if (!quiet) Console::Info("Checking drive readiness...\n");
	if (!WriteDiscInternal::WaitForDriveReady(drive, 15)) {
		if (!quiet) Console::Error("Drive did not become ready\n");
		return false;
	}
	if (!quiet) Console::Success("Drive is ready\n");

	if (!quiet) Console::Info("Configuring write parameters...\n");
	if (!SetWriteParametersPage(drive, subchannelMode, quiet)) {
		if (!quiet) Console::Error("Failed to configure write parameters\n");
		return false;
	}

	return true;
}