// ============================================================================
// ScsiDrive.Core.cpp - Core SCSI drive communication
// ============================================================================
#include "ScsiDrive.h"
#include <chrono>
#include <thread>
#include <vector>

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

	// FIX: Check both the IOCTL result AND the SCSI status byte.
	// ScsiStatus 0x00 = GOOD, 0x02 = CHECK CONDITION (command rejected by drive).
	// Without this check, failed SCSI commands appear successful — the buffer
	// stays zeroed, no drive activity occurs, and C2 scans report 0 errors.
	return result != 0 && sptd->ScsiStatus == 0;
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

	BYTE* sense = sptdBuffer.data() + sizeof(SCSI_PASS_THROUGH_DIRECT);
	if (senseKey) *senseKey = sense[2] & 0x0F;
	if (asc) *asc = sense[12];
	if (ascq) *ascq = sense[13];

	return result != 0 && sptd->ScsiStatus == 0;
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
	BYTE cdb[10] = { 0x5A, 0, 0x2A, 0, 0, 0, 0, 0, 28, 0 };
	BYTE buffer[28] = {};

	if (!SendSCSI(cdb, 10, buffer, 28, true))
		return false;

	readSpeed = (buffer[14] << 8) | buffer[15];
	writeSpeed = (buffer[20] << 8) | buffer[21];

	return true;
}

bool ScsiDrive::Eject() {
	if (m_handle == INVALID_HANDLE_VALUE) return false;
	DWORD bytesReturned;
	return DeviceIoControl(m_handle, IOCTL_STORAGE_EJECT_MEDIA,
		nullptr, 0, nullptr, 0, &bytesReturned, nullptr) != 0;
}

bool ScsiDrive::WaitForDriveReady(int timeoutSeconds) {
	auto start = std::chrono::steady_clock::now();

	while (true) {
		BYTE cdb[6] = { 0x00 };
		BYTE dummy[4] = {};
		BYTE senseKey = 0, asc = 0, ascq = 0;

		SendSCSIWithSense(cdb, 6, dummy, 0, &senseKey, &asc, &ascq, true);

		if (senseKey == 0) return true;

		if (senseKey == 0x06) {
			std::this_thread::sleep_for(std::chrono::milliseconds(100));
			continue;
		}

		if (senseKey == 0x02 && asc == 0x04) {
			std::this_thread::sleep_for(std::chrono::milliseconds(250));
			continue;
		}

		if (senseKey == 0x02 && asc != 0x3A) {
			std::this_thread::sleep_for(std::chrono::milliseconds(250));
			continue;
		}

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

	BYTE cdb[6] = { 0x00 };
	BYTE dummy[4] = {};
	BYTE senseKey = 0, asc = 0, ascq = 0;

	SendSCSIWithSense(cdb, 6, dummy, 0, &senseKey, &asc, &ascq, true);

	status.mediaReady = (senseKey == 0);
	status.mediaPresent = (senseKey != 0x02 || asc != 0x3A);
	status.trayOpen = (senseKey == 0x02 && asc == 0x3A && ascq == 0x02);
	status.spinningUp = (senseKey == 0x02 && asc == 0x04);

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