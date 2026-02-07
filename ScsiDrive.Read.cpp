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

bool ScsiDrive::ReadSectorWithC2(DWORD lba, BYTE* audio, BYTE* subchannel, int& c2Errors) {
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
	cdb[9] = 0xF8 | 0x02;
	cdb[10] = subchannel ? 0x01 : 0x00;

	if (!SendSCSI(cdb, 12, buffer.data(), bufferSize)) return false;

	memcpy(audio, buffer.data(), AUDIO_SECTOR_SIZE);

	c2Errors = 0;
	const BYTE* c2Data = buffer.data() + AUDIO_SECTOR_SIZE;
	for (int i = 0; i < 294; i++) {
		BYTE b = c2Data[i];
		while (b) { c2Errors += b & 1; b >>= 1; }
	}

	if (subchannel) {
		memcpy(subchannel, buffer.data() + AUDIO_SECTOR_SIZE + C2_ERROR_SIZE, SUBCHANNEL_SIZE);
	}
	return true;
}

bool ScsiDrive::ReadSectorWithC2Ex(DWORD lba, BYTE* audio, BYTE* subchannel,
	int& c2Errors, BYTE* c2Raw, const C2ReadOptions& options) {

	if (options.multiPass && options.passCount > 1) {
		return ReadSectorWithC2ExMultiPass(lba, audio, subchannel, c2Errors, c2Raw, options);
	}

	BYTE cdb[12] = {};
	int bufferSize = subchannel ? FULL_SECTOR_WITH_C2 : SECTOR_WITH_C2_SIZE;
	std::vector<BYTE> buffer(bufferSize);

	cdb[0] = SCSI_READ_CD;
	cdb[1] = 0x00;
	cdb[2] = (lba >> 24) & 0xFF;
	cdb[3] = (lba >> 16) & 0xFF;
	cdb[4] = (lba >> 8) & 0xFF;
	cdb[5] = lba & 0xFF;
	cdb[8] = 1;

	cdb[9] = 0xF8;
	cdb[10] = (subchannel ? 0x01 : 0x00) | 0xD8;

	bool useErrorBlock = false;
	if (!SendSCSI(cdb, 12, buffer.data(), bufferSize)) {
		cdb[9] = 0xF8 | 0x02;
		cdb[10] = subchannel ? 0x01 : 0x00;

		if (!SendSCSI(cdb, 12, buffer.data(), bufferSize)) {
			return false;
		}
		useErrorBlock = true;
	}

	memcpy(audio, buffer.data(), AUDIO_SECTOR_SIZE);

	c2Errors = 0;
	const BYTE* c2Data = buffer.data() + AUDIO_SECTOR_SIZE;

	if (useErrorBlock) {
		for (int i = 0; i < C2_ERROR_SIZE; i++) {
			if (options.countBytes) {
				if (c2Data[i] != 0) c2Errors++;
			} else {
				BYTE b = c2Data[i];
				while (b) { c2Errors += b & 1; b >>= 1; }
			}
		}
	} else {
		for (int i = 0; i < C2_ERROR_SIZE; i++) {
			BYTE b = c2Data[i];
			while (b) { c2Errors += b & 1; b >>= 1; }
		}
	}

	if (c2Raw) {
		memcpy(c2Raw, c2Data, C2_ERROR_SIZE);
	}

	if (subchannel) {
		memcpy(subchannel, buffer.data() + AUDIO_SECTOR_SIZE + C2_ERROR_SIZE, SUBCHANNEL_SIZE);
	}

	return true;
}

bool ScsiDrive::ReadSectorWithC2ExMultiPass(DWORD lba, BYTE* audio, BYTE* subchannel,
	int& c2Errors, BYTE* c2Raw, const C2ReadOptions& options) {

	std::vector<BYTE> bestAudio(AUDIO_SECTOR_SIZE);
	std::vector<BYTE> aggregatedC2(C2_ERROR_SIZE, 0);
	int minPassErrors = INT_MAX;

	for (int pass = 0; pass < options.passCount; pass++) {
		BYTE passAudio[AUDIO_SECTOR_SIZE];
		BYTE passC2[C2_ERROR_SIZE];
		int passErrors = 0;

		if (!ReadSectorWithC2Ex(lba, passAudio, subchannel, passErrors, passC2,
			C2ReadOptions{ false, 1, options.countBytes, false })) {
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

bool ScsiDrive::PlextorReadC2(DWORD lba, BYTE* audio, int& c2Errors) {
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
		BYTE b = c2Data[i];
		while (b) { c2Errors += b & 1; b >>= 1; }
	}

	return true;
}

bool ScsiDrive::ValidateC2Accuracy(DWORD testLBA) {
	std::vector<BYTE> audio(AUDIO_SECTOR_SIZE);
	int errors1 = 0, errors2 = 0;

	SetSpeed(4);
	if (!ReadSectorWithC2(testLBA, audio.data(), nullptr, errors1)) {
		return false;
	}

	SetSpeed(0);
	if (!ReadSectorWithC2(testLBA, audio.data(), nullptr, errors2)) {
		return false;
	}

	SetSpeed(0);

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

	int adr = qchannel[0] & 0x0F;
	if (adr != 1) return false;

	auto bcd2bin = [](BYTE b) { return ((b >> 4) & 0x0F) * 10 + (b & 0x0F); };
	qTrack = bcd2bin(qchannel[1]);
	qIndex = bcd2bin(qchannel[2]);
	return qTrack > 0 && qTrack <= 99;
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
	cdb[9] = 0x10;
	cdb[10] = 0x01;

	if (!SendSCSI(cdb, 12, buffer.data(), RAW_SECTOR_SIZE)) return false;
	return ParseRawSubchannel(buffer.data() + AUDIO_SECTOR_SIZE, qTrack, qIndex);
}

bool ScsiDrive::ReadSectorQ(DWORD lba, int& qTrack, int& qIndex) {
	if (ReadSectorQRaw(lba, qTrack, qIndex)) return true;

	BYTE cdb[12] = {};
	std::vector<BYTE> buffer(2368);
	cdb[0] = SCSI_READ_CD;
	cdb[1] = 0x00;
	cdb[2] = (lba >> 24) & 0xFF;
	cdb[3] = (lba >> 16) & 0xFF;
	cdb[4] = (lba >> 8) & 0xFF;
	cdb[5] = lba & 0xFF;
	cdb[8] = 1;
	cdb[9] = 0x10;
	cdb[10] = 0x02;

	if (!SendSCSI(cdb, 12, buffer.data(), 2368)) return false;

	BYTE* q = buffer.data() + AUDIO_SECTOR_SIZE;
	auto bcd2bin = [](BYTE bcd) { return ((bcd >> 4) & 0x0F) * 10 + (bcd & 0x0F); };
	qTrack = bcd2bin(q[1]);
	qIndex = bcd2bin(q[2]);
	return qTrack > 0 && qTrack < 100;
}