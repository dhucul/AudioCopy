// ============================================================================
// ScsiDrive.cpp - Low-level SCSI drive communication implementation
// ============================================================================
#include "ScsiDrive.h"
#include "DriveOffsets.h"
#include "OffsetCalibration.h"  // Add this line
#include <algorithm>
#include <chrono>
#include <thread>

bool ScsiDrive::Open(wchar_t driveLetter) {
	std::wstring path = L"\\\\.\\" + std::wstring(1, driveLetter) + L":";
	m_handle = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE,
		FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
	return m_handle != INVALID_HANDLE_VALUE;
}

void ScsiDrive::Close() {
	if (m_handle != INVALID_HANDLE_VALUE) {
		CloseHandle(m_handle);
		m_handle = INVALID_HANDLE_VALUE;
	}
}

bool ScsiDrive::SendSCSI(void* cdb, BYTE cdbLength, void* buffer, DWORD bufferSize, bool dataIn) {
	if (m_handle == INVALID_HANDLE_VALUE) return false;

	constexpr DWORD SENSE_SIZE = 32;
	std::vector<BYTE> sptdBuffer(sizeof(SCSI_PASS_THROUGH_DIRECT) + SENSE_SIZE);
	auto* sptd = reinterpret_cast<SCSI_PASS_THROUGH_DIRECT*>(sptdBuffer.data());
	ZeroMemory(sptd, sizeof(SCSI_PASS_THROUGH_DIRECT));

	sptd->Length = sizeof(SCSI_PASS_THROUGH_DIRECT);
	sptd->CdbLength = cdbLength;
	sptd->SenseInfoLength = SENSE_SIZE;
	sptd->DataIn = dataIn ? SCSI_IOCTL_DATA_IN : SCSI_IOCTL_DATA_OUT;
	sptd->DataTransferLength = bufferSize;
	sptd->TimeOutValue = 60;
	sptd->DataBuffer = buffer;
	sptd->SenseInfoOffset = sizeof(SCSI_PASS_THROUGH_DIRECT);
	memcpy(sptd->Cdb, cdb, cdbLength);

	DWORD bytesReturned;
	return DeviceIoControl(m_handle, IOCTL_SCSI_PASS_THROUGH_DIRECT,
		sptd, static_cast<DWORD>(sptdBuffer.size()),
		sptd, static_cast<DWORD>(sptdBuffer.size()),
		&bytesReturned, nullptr) != 0;
}

void ScsiDrive::SetSpeed(int multiplier) {
	m_currentSpeed = multiplier <= 0 ? CD_SPEED_MAX : static_cast<WORD>(multiplier * CD_SPEED_1X);

	BYTE cdb[12] = {};
	cdb[0] = SCSI_SET_CD_SPEED;
	cdb[2] = (m_currentSpeed >> 8) & 0xFF;
	cdb[3] = m_currentSpeed & 0xFF;
	cdb[4] = 0xFF;
	cdb[5] = 0xFF;
	BYTE dummy[4] = {};
	SendSCSI(cdb, 12, dummy, 0, false);
}

bool ScsiDrive::GetActualSpeed(WORD& readSpeed, WORD& writeSpeed) {
	// MODE SENSE (10) - Page 2A (Capabilities & Mechanical Status)
	BYTE cdb[10] = { 0x5A, 0, 0x2A, 0, 0, 0, 0, 0, 28, 0 };
	BYTE buffer[28] = {};

	if (!SendSCSI(cdb, 10, buffer, 28, true))
		return false;

	// Current read speed at offset 14-15 (KB/sec)
	readSpeed = (buffer[14] << 8) | buffer[15];
	// Current write speed at offset 20-21 (KB/sec)
	writeSpeed = (buffer[20] << 8) | buffer[21];

	return true;
}

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
// Insert after line 123 (after the closing brace of ReadSectorWithC2)

bool ScsiDrive::ReadSectorWithC2Ex(DWORD lba, BYTE* audio, BYTE* subchannel,
	int& c2Errors, BYTE* c2Raw, const C2ReadOptions& options) {
	
	// For most use cases, single-pass is sufficient and much faster
	// Multi-pass should only be used for verification/recovery, not scanning
	if (options.multiPass && options.passCount > 1) {
		return ReadSectorWithC2ExMultiPass(lba, audio, subchannel, c2Errors, c2Raw, options);
	}
	
	BYTE cdb[12] = {};
	int bufferSize = subchannel ? FULL_SECTOR_WITH_C2 : SECTOR_WITH_C2_SIZE;
	std::vector<BYTE> buffer(bufferSize);

	cdb[0] = SCSI_READ_CD;
	cdb[1] = 0x00;  // Use 0x00 instead of 0x04 - some drives misinterpret expected sector type
	cdb[2] = (lba >> 24) & 0xFF;
	cdb[3] = (lba >> 16) & 0xFF;
	cdb[4] = (lba >> 8) & 0xFF;
	cdb[5] = lba & 0xFF;
	cdb[8] = 1;
	
	// Try C2 error pointers first (0xD8) - more reliable on many drives
	cdb[9] = 0xF8;  // Audio data
	cdb[10] = (subchannel ? 0x01 : 0x00) | 0xD8;  // C2 error pointers

	bool useErrorBlock = false;
	if (!SendSCSI(cdb, 12, buffer.data(), bufferSize)) {
		// Fallback to C2 error block (0x02)
		cdb[9] = 0xF8 | 0x02;  // Audio + C2 block
		cdb[10] = subchannel ? 0x01 : 0x00;
		
		if (!SendSCSI(cdb, 12, buffer.data(), bufferSize)) {
			return false;
		}
		useErrorBlock = true;
	}

	memcpy(audio, buffer.data(), AUDIO_SECTOR_SIZE);

	// Count C2 errors based on format
	c2Errors = 0;
	const BYTE* c2Data = buffer.data() + AUDIO_SECTOR_SIZE;
	
	if (useErrorBlock) {
		// C2 error block: each bit represents an erroneous byte
		for (int i = 0; i < C2_ERROR_SIZE; i++) {
			if (options.countBytes) {
				if (c2Data[i] != 0) c2Errors++;
			} else {
				BYTE b = c2Data[i];
				while (b) { c2Errors += b & 1; b >>= 1; }
			}
		}
	} else {
		// C2 error pointers: bits point to specific sample positions
		// This is typically more accurate
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

// Separate multi-pass function - only use when necessary
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
        
        // Aggregate C2 data
        for (int i = 0; i < C2_ERROR_SIZE; i++) {
            aggregatedC2[i] |= passC2[i];
        }
        
        if (passErrors < minPassErrors) {
            memcpy(bestAudio.data(), passAudio, AUDIO_SECTOR_SIZE);
            minPassErrors = passErrors;
        }
    }
    
    // Recount from aggregated C2
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

// Add Plextor-specific C2 scanning
bool ScsiDrive::PlextorReadC2(DWORD lba, BYTE* audio, int& c2Errors) {
    // Plextor D8 command for accurate C2 detection
    BYTE cdb[12] = {};
    std::vector<BYTE> buffer(SECTOR_WITH_C2_SIZE);
    
    cdb[0] = 0xD8;  // Plextor READ CD DA
    cdb[2] = (lba >> 24) & 0xFF;
    cdb[3] = (lba >> 16) & 0xFF;
    cdb[4] = (lba >> 8) & 0xFF;
    cdb[5] = lba & 0xFF;
    cdb[8] = 1;  // Transfer length
    cdb[9] = 0x02;  // Include C2 errors
    
    if (!SendSCSI(cdb, 12, buffer.data(), SECTOR_WITH_C2_SIZE)) {
        return false;
    }
    
    memcpy(audio, buffer.data(), AUDIO_SECTOR_SIZE);
    
    // Count C2 errors
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

	// Test at slow speed
	SetSpeed(4);
	if (!ReadSectorWithC2(testLBA, audio.data(), nullptr, errors1)) {
		return false;
	}

	// Test at max speed
	SetSpeed(0);
	if (!ReadSectorWithC2(testLBA, audio.data(), nullptr, errors2)) {
		return false;
	}

	// Restore original speed
	SetSpeed(0);

	// Return true if consistent results (both detect or both don't)
	// Some drives report false C2 errors at high speeds
	return (errors1 > 0) == (errors2 > 0);
}

// Improved C2 support detection that checks multiple modes
bool ScsiDrive::CheckC2Support() {
    std::string vendor, model;
    GetDriveInfo(vendor, model);
    
    // Try Plextor command first if it's a Plextor drive
    if (vendor.find("PLEXTOR") != std::string::npos) {
        BYTE cdb[12] = { 0xD8, 0, 0, 0, 0, 0, 0, 0, 1, 0x02, 0, 0 };
        std::vector<BYTE> buffer(SECTOR_WITH_C2_SIZE);
        if (SendSCSI(cdb, 12, buffer.data(), SECTOR_WITH_C2_SIZE)) {
            m_c2Mode = C2Mode::PlextorD8;
            return true;
        }
    }
    
    // Try C2 error pointers (more accurate)
    BYTE cdb1[12] = { SCSI_READ_CD, 0x00, 0, 0, 0, 0, 0, 0, 1, 0xF8, 0xD8, 0 };
    std::vector<BYTE> buffer(SECTOR_WITH_C2_SIZE);
    if (SendSCSI(cdb1, 12, buffer.data(), SECTOR_WITH_C2_SIZE)) {
        m_c2Mode = C2Mode::ErrorPointers;
        return true;
    }
    
    // Try C2 error block (standard)
    BYTE cdb2[12] = { SCSI_READ_CD, 0x04, 0, 0, 0, 0, 0, 0, 1, 0xFA, 0x00, 0 };
    if (SendSCSI(cdb2, 12, buffer.data(), SECTOR_WITH_C2_SIZE)) {
        m_c2Mode = C2Mode::ErrorBlock;
        return true;
    }
    
    m_c2Mode = C2Mode::NotSupported;
    return false;
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



bool ScsiDrive::GetDriveInfo(std::string& vendor, std::string& model) {
	BYTE cdb[6] = { 0x12, 0, 0, 0, 96, 0 };
	std::vector<BYTE> buffer(96, 0);
	if (!SendSCSI(cdb, 6, buffer.data(), 96)) return false;

	vendor = std::string(reinterpret_cast<char*>(&buffer[8]), 8);
	model = std::string(reinterpret_cast<char*>(&buffer[16]), 16);
	while (!vendor.empty() && vendor.back() == ' ') vendor.pop_back();
	while (!model.empty() && model.back() == ' ') model.pop_back();
	return true;
}

bool ScsiDrive::Eject() {
	if (m_handle == INVALID_HANDLE_VALUE) return false;
	DWORD bytesReturned;
	return DeviceIoControl(m_handle, IOCTL_STORAGE_EJECT_MEDIA,
		nullptr, 0, nullptr, 0, &bytesReturned, nullptr) != 0;
}

bool ScsiDrive::SendSCSIWithSense(void* cdb, BYTE cdbLength, void* buffer, DWORD bufferSize,
	BYTE* senseKey, BYTE* asc, BYTE* ascq, bool dataIn) {
	if (m_handle == INVALID_HANDLE_VALUE) {
		if (senseKey) *senseKey = 0x02;  // Not Ready
		return false;
	}

	constexpr DWORD SENSE_SIZE = 32;
	std::vector<BYTE> sptdBuffer(sizeof(SCSI_PASS_THROUGH_DIRECT) + SENSE_SIZE);
	auto* sptd = reinterpret_cast<SCSI_PASS_THROUGH_DIRECT*>(sptdBuffer.data());
	ZeroMemory(sptd, sptdBuffer.size());

	sptd->Length = sizeof(SCSI_PASS_THROUGH_DIRECT);
	sptd->CdbLength = cdbLength;
	sptd->SenseInfoLength = SENSE_SIZE;
	sptd->DataIn = dataIn ? SCSI_IOCTL_DATA_IN : SCSI_IOCTL_DATA_OUT;
	sptd->DataTransferLength = bufferSize;
	sptd->TimeOutValue = 60;
	sptd->DataBuffer = buffer;
	sptd->SenseInfoOffset = sizeof(SCSI_PASS_THROUGH_DIRECT);
	memcpy(sptd->Cdb, cdb, cdbLength);

	DWORD bytesReturned;
	BOOL result = DeviceIoControl(m_handle, IOCTL_SCSI_PASS_THROUGH_DIRECT,
		sptd, static_cast<DWORD>(sptdBuffer.size()),
		sptd, static_cast<DWORD>(sptdBuffer.size()),
		&bytesReturned, nullptr);

	// Extract sense data
	BYTE* sense = sptdBuffer.data() + sizeof(SCSI_PASS_THROUGH_DIRECT);
	if (senseKey) *senseKey = sense[2] & 0x0F;
	if (asc) *asc = sense[12];
	if (ascq) *ascq = sense[13];

	return result != 0 && (sense[2] & 0x0F) == 0;  // No sense = success
}

bool ScsiDrive::WaitForDriveReady(int timeoutSeconds) {
	auto start = std::chrono::steady_clock::now();

	while (true) {
		BYTE cdb[6] = { 0x00 };  // TEST UNIT READY
		BYTE dummy[4] = {};
		BYTE senseKey = 0, asc = 0, ascq = 0;

		SendSCSIWithSense(cdb, 6, dummy, 0, &senseKey, &asc, &ascq, true);

		// Drive ready with media
		if (senseKey == 0) return true;

		// Unit Attention (media changed) - clear it and retry quickly
		if (senseKey == 0x06) {
			std::this_thread::sleep_for(std::chrono::milliseconds(100));
			continue;
		}

		// Drive spinning up (ASC 0x04 = Logical Unit Not Ready)
		if (senseKey == 0x02 && asc == 0x04) {
			std::this_thread::sleep_for(std::chrono::milliseconds(250));
			continue;
		}

		// Media present but not ready (other transition states)
		if (senseKey == 0x02 && asc != 0x3A) {
			std::this_thread::sleep_for(std::chrono::milliseconds(250));
			continue;
		}

		// Check timeout
		auto now = std::chrono::steady_clock::now();
		auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - start).count();
		if (elapsed >= timeoutSeconds) return false;

		std::this_thread::sleep_for(std::chrono::milliseconds(500));
	}
}

bool ScsiDrive::GetMediaStatus(DriveHealthCheck& status) {
	status = DriveHealthCheck{};
	status.driveResponding = IsOpen();
	if (!status.driveResponding) return false;

	// Test Unit Ready to check media state
	BYTE cdb[6] = { 0x00 };
	BYTE dummy[4] = {};
	BYTE senseKey = 0, asc = 0, ascq = 0;

	SendSCSIWithSense(cdb, 6, dummy, 0, &senseKey, &asc, &ascq, true);

	status.mediaReady = (senseKey == 0);
	status.mediaPresent = (senseKey != 0x02 || asc != 0x3A);  // Not "Medium not present"
	status.trayOpen = (senseKey == 0x02 && asc == 0x3A && ascq == 0x02);
	status.spinningUp = (senseKey == 0x02 && asc == 0x04);

	// Get media type via READ TOC
	if (status.mediaPresent && status.mediaReady) {
		BYTE tocCdb[10] = { 0x43, 0x00, 0, 0, 0, 0, 0, 0, 4, 0 };
		std::vector<BYTE> toc(4);
		if (SendSCSI(tocCdb, 10, toc.data(), 4)) {
			status.mediaType = "CD";
		}
	}

	return true;
}

std::string ScsiDrive::GetSenseDescription(BYTE senseKey, BYTE asc, BYTE ascq) {
	// Common SCSI sense codes for CD drives
	switch (senseKey) {
	case 0x00: return "No Sense";
	case 0x01: return "Recovered Error";
	case 0x02:
		if (asc == 0x04) return "Drive Not Ready (spinning up)";
		if (asc == 0x3A) return "Medium Not Present";
		return "Not Ready";
	case 0x03:
		if (asc == 0x02) return "No Seek Complete";
		if (asc == 0x11) return "Unrecovered Read Error";
		if (asc == 0x14) return "Track Following Error";
		return "Medium Error";
	case 0x04: return "Hardware Error";
	case 0x05: return "Illegal Request";
	case 0x06: return "Unit Attention (media changed)";
	case 0x0B: return "Aborted Command";
	default: return "Unknown Error (0x" + std::to_string(senseKey) + ")";
	}
}

bool ScsiDrive::GetModePage2A(std::vector<BYTE>& pageData) {
	// MODE SENSE (10) - Get CD/DVD Capabilities page (0x2A)
	BYTE cdb[10] = {};
	cdb[0] = 0x5A;          // MODE SENSE (10)
	cdb[2] = 0x2A;          // Page code: CD/DVD Capabilities
	cdb[7] = 0x00;          // Allocation length MSB
	cdb[8] = 0xFF;          // Allocation length LSB (255 bytes)

	pageData.resize(255, 0);
	if (!SendSCSI(cdb, 10, pageData.data(), 255)) {
		// Try MODE SENSE (6) as fallback
		BYTE cdb6[6] = {};
		cdb6[0] = 0x1A;     // MODE SENSE (6)
		cdb6[2] = 0x2A;     // Page code
		cdb6[4] = 0xFF;     // Allocation length

		if (!SendSCSI(cdb6, 6, pageData.data(), 255)) {
			return false;
		}
	}
	return true;
}

bool ScsiDrive::TestOverread(bool leadIn) {
	// Try to read a sector in the lead-in or lead-out area
	BYTE audio[AUDIO_SECTOR_SIZE];
	DWORD testLBA = leadIn ? 0xFFFFFF00 : 0;  // Negative LBA for lead-in

	if (leadIn) {
		// Lead-in is at negative LBAs (before LBA 0)
		// Use a small negative offset to test
		BYTE cdb[12] = {};
		cdb[0] = SCSI_READ_CD;
		cdb[1] = 0x04;  // Expected sector type: CDDA
		// LBA -150 (pregap area)
		DWORD negLBA = static_cast<DWORD>(-150);
		cdb[2] = (negLBA >> 24) & 0xFF;
		cdb[3] = (negLBA >> 16) & 0xFF;
		cdb[4] = (negLBA >> 8) & 0xFF;
		cdb[5] = negLBA & 0xFF;
		cdb[8] = 1;     // Transfer length
		cdb[9] = 0x10;  // User data only
		return SendSCSI(cdb, 12, audio, AUDIO_SECTOR_SIZE);
	}
	else {
		// For lead-out, we'd need to know the disc length
		// This is a basic test - actual implementation would use TOC
		return false;
	}
}

bool ScsiDrive::DetectCapabilities(DriveCapabilities& caps) {
	caps = DriveCapabilities{};

	// Get basic drive info via INQUIRY
	BYTE inqCdb[6] = { 0x12, 0, 0, 0, 96, 0 };
	std::vector<BYTE> inqBuffer(96, 0);
	if (SendSCSI(inqCdb, 6, inqBuffer.data(), 96)) {
		caps.vendor = std::string(reinterpret_cast<char*>(&inqBuffer[8]), 8);
		caps.model = std::string(reinterpret_cast<char*>(&inqBuffer[16]), 16);
		caps.firmware = std::string(reinterpret_cast<char*>(&inqBuffer[32]), 4);

		auto trim = [](std::string& s) {
			while (!s.empty() && (s.back() == ' ' || s.back() == '\0')) s.pop_back();
		};
		trim(caps.vendor);
		trim(caps.model);
		trim(caps.firmware);
	}

	// Get serial number from VPD page 0x80
	BYTE vpd80Cdb[6] = { 0x12, 0x01, 0x80, 0, 64, 0 };
	std::vector<BYTE> vpd80Buffer(64, 0);
	if (SendSCSI(vpd80Cdb, 6, vpd80Buffer.data(), 64)) {
		int len = vpd80Buffer[3];
		if (len > 0 && len < 60) {
			caps.serialNumber = std::string(reinterpret_cast<char*>(&vpd80Buffer[4]), len);
			while (!caps.serialNumber.empty() && caps.serialNumber.back() == ' ')
				caps.serialNumber.pop_back();
		}
	}

	// Get Mode Page 2A (CD/DVD Capabilities and Mechanical Status)
	std::vector<BYTE> pageData;
	if (GetModePage2A(pageData)) {
		size_t offset = (pageData[0] == 0) ? 8 : 4;

		while (offset < pageData.size() - 2) {
			if ((pageData[offset] & 0x3F) == 0x2A) {
				BYTE* page = &pageData[offset];
				int pageLen = page[1];

				if (pageLen >= 4) {
					// Byte 2: Read capabilities
					caps.readsCDR   = (page[2] & 0x01) != 0;
					caps.readsCDRW  = (page[2] & 0x02) != 0;
					caps.readsDVD   = (page[2] & 0x08) != 0;

					// Byte 3: Write capabilities
					caps.writesCDR  = (page[3] & 0x01) != 0;
					caps.writesCDRW = (page[3] & 0x02) != 0;
					caps.writesDVD  = (page[3] & 0x10) != 0;

					// Byte 4: Audio features
					caps.supportsCDDA = (page[4] & 0x01) != 0;
					caps.supportsAccurateStream = (page[4] & 0x02) != 0;
					caps.supportsSubchannelRaw = (page[4] & 0x04) != 0;
					caps.supportsSubchannelQ = (page[4] & 0x08) != 0;
					caps.supportsCDText = (page[4] & 0x40) != 0;
					caps.supportsDigitalAudioPlay = (page[4] & 0x80) != 0;

					// Byte 5: More capabilities
					caps.supportsSeparateVolume = (page[5] & 0x01) != 0;
					caps.supportsCompositeOutput = (page[5] & 0x02) != 0;
					caps.supportsSeparateMute = (page[5] & 0x08) != 0;
					caps.supportsMultiSession = (page[5] & 0x40) != 0;

					// Byte 6: Mechanical features
					caps.loadingMechanism = (page[6] >> 5) & 0x07;
					caps.supportsEject = (page[6] & 0x08) != 0;
					caps.supportsLockMedia = (page[6] & 0x01) != 0;
					caps.isChanger = (page[6] & 0x10) != 0;
				}

				if (pageLen >= 14) {
					caps.maxReadSpeedKB = (page[8] << 8) | page[9];
					caps.currentReadSpeedKB = (page[14] << 8) | page[15];
					if (pageLen >= 20) {
						caps.maxWriteSpeedKB = (page[18] << 8) | page[19];
					}
				}

				if (pageLen >= 12) {
					caps.bufferSizeKB = (page[12] << 8) | page[13];
				}
				break;
			}
			offset += 2 + pageData[offset + 1];
		}
	}

	// Get supported speeds via GET PERFORMANCE
	BYTE perfCdb[12] = { 0xAC, 0x10, 0, 0, 0, 0, 0, 0, 0, 32, 0x03, 0 };
	std::vector<BYTE> perfBuffer(32 + 8, 0);
	if (SendSCSI(perfCdb, 12, perfBuffer.data(), static_cast<int>(perfBuffer.size()))) {
		int dataLen = (perfBuffer[0] << 24) | (perfBuffer[1] << 16) |
			(perfBuffer[2] << 8) | perfBuffer[3];
		int numDescriptors = (dataLen - 4) / 16;
		for (int i = 0; i < numDescriptors && i < 8; i++) {
			int off = 8 + i * 16;
			int speed = (perfBuffer[off + 12] << 24) | (perfBuffer[off + 13] << 16) |
				(perfBuffer[off + 14] << 8) | perfBuffer[off + 15];
			if (speed > 0) {
				caps.supportedReadSpeeds.push_back(speed);
			}
		}
	}

	// Check media presence
	DWORD ret;
	caps.mediaPresent = DeviceIoControl(m_handle, IOCTL_STORAGE_CHECK_VERIFY,
		nullptr, 0, nullptr, 0, &ret, nullptr) != 0;

	// Test C2, raw read, and overread capabilities
	caps.supportsC2ErrorReporting = CheckC2Support();

	BYTE rawBuffer[AUDIO_SECTOR_SIZE];
	BYTE rawCdb[12] = {};
	rawCdb[0] = SCSI_READ_CD;
	rawCdb[1] = 0x04;
	rawCdb[8] = 1;
	rawCdb[9] = 0xF8;
	caps.supportsRawRead = SendSCSI(rawCdb, 12, rawBuffer, AUDIO_SECTOR_SIZE);

	caps.supportsOverreadLeadIn = TestOverread(true);
	caps.supportsOverreadLeadOut = TestOverread(false);

	return true;
}

bool ScsiDrive::DetectDriveOffset(OffsetDetectionResult& result) {
	// 1. Check AccurateRip database first (highest confidence for known drives)
	DriveOffsetInfo dbInfo;
	if (LookupAccurateRipOffset(dbInfo)) {
		result.offset = dbInfo.readOffset;
		result.confidence = 95;
		result.method = OffsetDetectionMethod::Database;
		result.details = dbInfo.source;
		return true;
	}

	// 2. Check drive capabilities (Mode Page 2A)
	DriveCapabilities caps;
	if (DetectCapabilities(caps)) {
		if (caps.supportsAccurateStream) {
			result.details += "Drive supports Accurate Stream. ";
		}
	}

	// 3. Try auto-calibration with disc if media is present
	if (caps.mediaPresent) {
		OffsetCalibration calibration(*this);

		CalibrationResult calResult = calibration.QuickCalibrate(nullptr);

		if (calResult.success && calResult.confidence >= 70) {
			result.offset = calResult.detectedOffset;
			result.confidence = calResult.confidence;
			result.method = OffsetDetectionMethod::AccurateRipCalibration;
			result.details = "Auto-calibrated using AccurateRip (" +
				std::to_string(calResult.matchingTracks) + "/" +
				std::to_string(calResult.totalTracks) + " tracks matched)";
			return true;
		}

		// 4. Fallback to pregap analysis
		int pregapOffset = 0;
		if (DetectOffsetFromPregap(0, pregapOffset)) {
			result.offset = pregapOffset;
			result.confidence = 50;
			result.method = OffsetDetectionMethod::PregapAnalysis;
			result.details = "Estimated from pregap analysis (less reliable)";
			return true;
		}
	}

	// 5. Default: unknown - manual calibration needed
	result.offset = 0;
	result.confidence = 0;
	result.method = OffsetDetectionMethod::Unknown;
	result.details = "Unknown drive - insert a disc from AccurateRip database for auto-calibration";
	return false;
}

bool ScsiDrive::LookupAccurateRipOffset(DriveOffsetInfo& info) {
	std::string vendor, model;
	if (!GetDriveInfo(vendor, model)) return false;

	// Use the knownOffsets array from DriveOffsets.h
	for (int i = 0; knownOffsets[i].vendor != nullptr; i++) {
		// Case-insensitive partial match for vendor
		std::string upperVendor = vendor;
		std::string upperDbVendor = knownOffsets[i].vendor;
		for (char& c : upperVendor) c = toupper(c);
		for (char& c : upperDbVendor) c = toupper(c);

		if (upperVendor.find(upperDbVendor) != std::string::npos &&
			model.find(knownOffsets[i].model) != std::string::npos) {
			info.readOffset = knownOffsets[i].offset;
			info.fromDatabase = true;
			info.source = "AccurateRip Database";
			return true;
		}
	}
	return false;
}

bool ScsiDrive::DetectOffsetFromPregap(int trackStartLBA, int& estimatedOffset) {
	constexpr int SCAN_RANGE = 20;
	constexpr int SILENCE_THRESHOLD = 16;
	constexpr int MAX_REASONABLE_OFFSET = 1200;  // Most drives are within ±1200 samples

	int transitionPoint = 0;
	bool foundSilence = false;
	bool foundAudio = false;

	for (int i = -SCAN_RANGE; i < SCAN_RANGE; i++) {
		BYTE sector[AUDIO_SECTOR_SIZE];
		if (!ReadSectorAudioOnly(trackStartLBA + i, sector)) continue;

		int16_t* samples = reinterpret_cast<int16_t*>(sector);
		int silentSamples = 0;

		for (int s = 0; s < AUDIO_SECTOR_SIZE / 2; s++) {
			if (std::abs(samples[s]) < SILENCE_THRESHOLD) silentSamples++;
		}

		bool isSilent = silentSamples > (AUDIO_SECTOR_SIZE / 4);

		if (isSilent && !foundSilence) {
			foundSilence = true;
		}
		else if (!isSilent && foundSilence && !foundAudio) {
			// Found silence-to-audio transition
			foundAudio = true;
			transitionPoint = i * 588;
		}
	}

	// Validate the offset is within reasonable bounds
	if (foundSilence && foundAudio && std::abs(transitionPoint) <= MAX_REASONABLE_OFFSET) {
		estimatedOffset = transitionPoint;
		return true;
	}

	// Offset outside reasonable range - don't trust pregap analysis
	estimatedOffset = 0;
	return false;
}