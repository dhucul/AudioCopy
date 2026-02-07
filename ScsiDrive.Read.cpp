// ============================================================================
// ScsiDrive.Read.cpp - SCSI sector reading and C2 handling
// ============================================================================
#include "ScsiDrive.h"
#include <climits>
#include <vector>

bool ScsiDrive::ReadSector(DWORD lba, BYTE* audio, BYTE* subchannel) {
	BYTE cdb[12] = {};
	std::vector<BYTE> buffer(RAW_SECTOR_SIZE);

	cdb[0] = SCSI_READ_CD;
	cdb[1] = 0x04;
	cdb[2] = (lba >> 24) & 0xFF;
	cdb[3] = (lba >> 16) & 0xFF;
	cdb[4] = (lba >> 8) & 0xFF;
	cdb[5] = lba & 0xFF;
	cdb[8] = 1;
	cdb[9] = 0xF8;
	cdb[10] = 0x01;

	if (!SendSCSI(cdb, 12, buffer.data(), RAW_SECTOR_SIZE)) return false;

	memcpy(audio, buffer.data(), AUDIO_SECTOR_SIZE);
	memcpy(subchannel, buffer.data() + AUDIO_SECTOR_SIZE, SUBCHANNEL_SIZE);
	return true;
}

// FIX A: Delegate to ReadSectorWithC2Ex so m_c2Mode is respected everywhere.
// This fixes RunDiscRotScan, RunSpeedComparisonTest, CheckLeadAreas,
// GenerateSurfaceMap, and ScanDiscForC2Errors dual-speed validation.
bool ScsiDrive::ReadSectorWithC2(DWORD lba, BYTE* audio, BYTE* subchannel, int& c2Errors) {
	C2ReadOptions opts;
	return ReadSectorWithC2Ex(lba, audio, subchannel, c2Errors, nullptr, opts);
}

bool ScsiDrive::ReadSectorWithC2Ex(DWORD lba, BYTE* audio, BYTE* subchannel,
	int& c2Errors, BYTE* c2Raw, const C2ReadOptions& options) {

	if (options.multiPass && options.passCount > 1) {
		return ReadSectorWithC2ExMultiPass(lba, audio, subchannel, c2Errors, c2Raw, options);
	}

	// Use Plextor vendor command when detected
	if (m_c2Mode == C2Mode::PlextorD8) {
		return PlextorReadC2(lba, audio, c2Errors, c2Raw, options.countBytes);
	}

	BYTE cdb[12] = {};
	int bufferSize = subchannel ? FULL_SECTOR_WITH_C2 : SECTOR_WITH_C2_SIZE;
	std::vector<BYTE> buffer(bufferSize);

	cdb[0] = SCSI_READ_CD;
	cdb[1] = 0x04;
	cdb[2] = (lba >> 24) & 0xFF;
	cdb[3] = (lba >> 16) & 0xFF;
	cdb[4] = (lba >> 8) & 0xFF;
	cdb[5] = lba & 0xFF;
	cdb[8] = 1;

	// Use detected mode, or try C2+block error bits first, fallback to C2 block
	bool useErrorBlock = false;

	if (m_c2Mode == C2Mode::ErrorBlock) {
		cdb[9] = 0xF8 | 0x02;  // C2 bits = 01b
		cdb[10] = subchannel ? 0x01 : 0x00;
		useErrorBlock = true;
		if (!SendSCSI(cdb, 12, buffer.data(), bufferSize)) {
			return false;
		}
	}
	else {
		// ErrorPointers or unknown — try 10b first, fallback to 01b
		cdb[9] = 0xF8 | 0x04;  // C2 bits = 10b → C2 and block error bits
		cdb[10] = subchannel ? 0x01 : 0x00;

		if (!SendSCSI(cdb, 12, buffer.data(), bufferSize)) {
			cdb[9] = 0xF8 | 0x02;  // C2 bits = 01b → C2 error block
			cdb[10] = subchannel ? 0x01 : 0x00;

			if (!SendSCSI(cdb, 12, buffer.data(), bufferSize)) {
				return false;
			}
			useErrorBlock = true;
		}
	}

	memcpy(audio, buffer.data(), AUDIO_SECTOR_SIZE);

	c2Errors = 0;
	const BYTE* c2Data = buffer.data() + AUDIO_SECTOR_SIZE;

	// C2 error block = 294 bytes, C2+block error bits = 296 bytes
	int c2ByteCount = useErrorBlock ? 294 : C2_ERROR_SIZE;
	for (int i = 0; i < c2ByteCount; i++) {
		if (options.countBytes) {
			if (c2Data[i] != 0) c2Errors++;
		} else {
			BYTE b = c2Data[i];
			while (b) { c2Errors += b & 1; b >>= 1; }
		}
	}

	if (c2Raw) {
		memset(c2Raw, 0, C2_ERROR_SIZE);
		memcpy(c2Raw, c2Data, c2ByteCount);
	}

	if (subchannel) {
		memcpy(subchannel, buffer.data() + AUDIO_SECTOR_SIZE + C2_ERROR_SIZE, SUBCHANNEL_SIZE);
	}

	return true;
}

// FIX B: Cache defeat between multi-pass reads
bool ScsiDrive::ReadSectorWithC2ExMultiPass(DWORD lba, BYTE* audio, BYTE* subchannel,
	int& c2Errors, BYTE* c2Raw, const C2ReadOptions& options) {

	std::vector<BYTE> bestAudio(AUDIO_SECTOR_SIZE);
	std::vector<BYTE> aggregatedC2(C2_ERROR_SIZE, 0);
	int minPassErrors = INT_MAX;

	for (int pass = 0; pass < options.passCount; pass++) {
		// Cache defeat: seek to a distant sector between passes
		if (options.defeatCache && pass > 0) {
			BYTE dummy[AUDIO_SECTOR_SIZE];
			DWORD cacheBustLBA = (lba > 10000) ? lba - 10000 : lba + 10000;
			ReadSectorAudioOnly(cacheBustLBA, dummy);
		}

		BYTE passAudio[AUDIO_SECTOR_SIZE];
		BYTE passC2[C2_ERROR_SIZE];
		int passErrors = 0;

		C2ReadOptions singleOpts;
		singleOpts.multiPass = false;
		singleOpts.passCount = 1;
		singleOpts.defeatCache = false;
		singleOpts.countBytes = options.countBytes;

		if (!ReadSectorWithC2Ex(lba, passAudio, subchannel, passErrors, passC2, singleOpts)) {
			return false;
		}

		for (int i = 0; i < C2_ERROR_SIZE; i++) {
			aggregatedC2[i] |= passC2[i];
		}

		if (passErrors < minPassErrors) {
			memcpy(bestAudio.data(), passAudio, AUDIO_SECTOR_SIZE);
			minPassErrors = passErrors;
		}
	}

	c2Errors = 0;
	for (int i = 0; i < C2_ERROR_SIZE; i++) {
		if (options.countBytes) {
			if (aggregatedC2[i] != 0) c2Errors++;
		} else {
			BYTE b = aggregatedC2[i];
			while (b) { c2Errors += b & 1; b >>= 1; }
		}
	}

	memcpy(audio, bestAudio.data(), AUDIO_SECTOR_SIZE);
	if (c2Raw) memcpy(c2Raw, aggregatedC2.data(), C2_ERROR_SIZE);

	return true;
}

bool ScsiDrive::PlextorReadC2(DWORD lba, BYTE* audio, int& c2Errors, BYTE* c2Raw, bool countBytes) {
	BYTE cdb[12] = {};
	std::vector<BYTE> buffer(SECTOR_WITH_C2_SIZE);

	cdb[0] = 0xD8;
	cdb[2] = (lba >> 24) & 0xFF;
	cdb[3] = (lba >> 16) & 0xFF;
	cdb[4] = (lba >> 8) & 0xFF;
	cdb[5] = lba & 0xFF;
	cdb[8] = 1;
	cdb[9] = 0x02;

	if (!SendSCSI(cdb, 12, buffer.data(), SECTOR_WITH_C2_SIZE)) {
		return false;
	}

	memcpy(audio, buffer.data(), AUDIO_SECTOR_SIZE);

	c2Errors = 0;
	const BYTE* c2Data = buffer.data() + AUDIO_SECTOR_SIZE;
	for (int i = 0; i < C2_ERROR_SIZE; i++) {
		if (countBytes) {
			if (c2Data[i] != 0) c2Errors++;
		} else {
			BYTE b = c2Data[i];
			while (b) { c2Errors += b & 1; b >>= 1; }
		}
	}

	if (c2Raw) {
		memcpy(c2Raw, c2Data, C2_ERROR_SIZE);
	}

	return true;
}

// FIX C & D: Restore missing functions (unresolved externals)

bool ScsiDrive::ValidateC2Accuracy(DWORD testLBA) {
	std::vector<BYTE> audio(AUDIO_SECTOR_SIZE);
	int errors1 = 0, errors2 = 0;

	SetSpeed(4);
	C2ReadOptions opts;
	if (!ReadSectorWithC2Ex(testLBA, audio.data(), nullptr, errors1, nullptr, opts)) {
		SetSpeed(0);
		return false;
	}

	SetSpeed(0);
	if (!ReadSectorWithC2Ex(testLBA, audio.data(), nullptr, errors2, nullptr, opts)) {
		return false;
	}

	return (errors1 > 0) == (errors2 > 0);
}

bool ScsiDrive::ReadSectorAudioOnly(DWORD lba, BYTE* audio) {
	BYTE cdb[12] = {};
	cdb[0] = SCSI_READ_CD;
	cdb[1] = 0x04;
	cdb[2] = (lba >> 24) & 0xFF;
	cdb[3] = (lba >> 16) & 0xFF;
	cdb[4] = (lba >> 8) & 0xFF;
	cdb[5] = lba & 0xFF;
	cdb[8] = 1;
	cdb[9] = 0xF8;
	cdb[10] = 0x00;
	return SendSCSI(cdb, 12, audio, AUDIO_SECTOR_SIZE);
}

bool ScsiDrive::ReadDataSector(DWORD lba, BYTE* data) {
	BYTE cdb[12] = {};
	cdb[0] = SCSI_READ_CD;
	cdb[1] = 0x00;
	cdb[2] = (lba >> 24) & 0xFF;
	cdb[3] = (lba >> 16) & 0xFF;
	cdb[4] = (lba >> 8) & 0xFF;
	cdb[5] = lba & 0xFF;
	cdb[8] = 1;
	cdb[9] = 0xF8;
	cdb[10] = 0x00;
	return SendSCSI(cdb, 12, data, AUDIO_SECTOR_SIZE);
}

bool ScsiDrive::ParseRawSubchannel(const BYTE* sub, int& qTrack, int& qIndex) {
	BYTE qchannel[12] = {};
	for (int i = 0; i < 96; i++) {
		int byteIdx = i / 8;
		int bitIdx = 7 - (i % 8);
		if (sub[i] & 0x40) {
			qchannel[byteIdx] |= (1 << bitIdx);
		}
	}

	qTrack = qchannel[1];
	qIndex = qchannel[2];
	return true;
}

bool ScsiDrive::ReadSectorQRaw(DWORD lba, int& qTrack, int& qIndex) {
	BYTE cdb[12] = {};
	std::vector<BYTE> buffer(RAW_SECTOR_SIZE);

	cdb[0] = SCSI_READ_CD;
	cdb[1] = 0x04;
	cdb[2] = (lba >> 24) & 0xFF;
	cdb[3] = (lba >> 16) & 0xFF;
	cdb[4] = (lba >> 8) & 0xFF;
	cdb[5] = lba & 0xFF;
	cdb[8] = 1;
	cdb[9] = 0xF8;
	cdb[10] = 0x01;  // Raw subchannel

	if (!SendSCSI(cdb, 12, buffer.data(), RAW_SECTOR_SIZE)) return false;

	return ParseRawSubchannel(buffer.data() + AUDIO_SECTOR_SIZE, qTrack, qIndex);
}

bool ScsiDrive::ReadSectorQ(DWORD lba, int& qTrack, int& qIndex) {
	if (ReadSectorQRaw(lba, qTrack, qIndex)) {
		return true;
	}

	// Fallback: formatted Q subchannel
	BYTE cdb[12] = {};
	std::vector<BYTE> buffer(AUDIO_SECTOR_SIZE + 16);

	cdb[0] = SCSI_READ_CD;
	cdb[1] = 0x04;
	cdb[2] = (lba >> 24) & 0xFF;
	cdb[3] = (lba >> 16) & 0xFF;
	cdb[4] = (lba >> 8) & 0xFF;
	cdb[5] = lba & 0xFF;
	cdb[8] = 1;
	cdb[9] = 0xF8;
	cdb[10] = 0x02;  // Formatted Q subchannel

	if (!SendSCSI(cdb, 12, buffer.data(), AUDIO_SECTOR_SIZE + 16)) return false;

	const BYTE* qData = buffer.data() + AUDIO_SECTOR_SIZE;
	qTrack = qData[1];
	qIndex = qData[2];
	return true;
}