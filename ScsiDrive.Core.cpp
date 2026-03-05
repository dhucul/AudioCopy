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

	if (!result) return false;

	// SCSI GOOD (0x00) — command completed normally
	if (sptd->ScsiStatus == 0) return true;

	// CHECK CONDITION (0x02) — inspect sense key to decide if buffer is valid.
	// Sense 0x00 (No Sense) and 0x01 (Recovered Error) both mean the drive
	// completed the command and the data buffer is valid. Many Plextor drives
	// return CHECK CONDITION with 0x00 on every vendor D8 read.
	if (sptd->ScsiStatus == 0x02) {
		BYTE* sense = sptdBuffer.data() + sizeof(SCSI_PASS_THROUGH_DIRECT);
		BYTE sk = sense[2] & 0x0F;
		return sk <= 0x01;
	}

	return false;
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

	// FIX: When buffer is null or size is 0, use SCSI_IOCTL_DATA_UNSPECIFIED
	// (no data transfer).  Using DATA_OUT with a null buffer causes some
	// SCSI miniport drivers to silently fail or drop the command entirely.
	if (buffer == nullptr || bufferSize == 0) {
		sptd->DataIn = SCSI_IOCTL_DATA_UNSPECIFIED;
		sptd->DataTransferLength = 0;
		sptd->DataBuffer = nullptr;
	}
	else {
		sptd->DataIn = dataIn ? SCSI_IOCTL_DATA_IN : SCSI_IOCTL_DATA_OUT;
		sptd->DataTransferLength = bufferSize;
		sptd->DataBuffer = buffer;
	}

	sptd->TimeOutValue = 60;
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

	if (!result) return false;

	// GOOD status — no error at all
	if (sptd->ScsiStatus == 0) return true;

	// CHECK CONDITION — sense 0x00/0x01 = data buffer is valid
	if (sptd->ScsiStatus == 0x02) {
		BYTE sk = sense[2] & 0x0F;
		return sk <= 0x01;
	}

	return false;
}

void ScsiDrive::SetSpeed(int multiplier, int writeMultiplier) {
	m_currentSpeed = multiplier <= 0 ? CD_SPEED_MAX : static_cast<WORD>(multiplier * CD_SPEED_1X);

	// If no explicit write speed given, mirror the read speed
	WORD writeSpeed = CD_SPEED_MAX;
	if (writeMultiplier > 0)
		writeSpeed = static_cast<WORD>(writeMultiplier * CD_SPEED_1X);
	else if (multiplier > 0)
		writeSpeed = m_currentSpeed;

	BYTE cdb[12] = {};
	cdb[0] = SCSI_SET_CD_SPEED;
	cdb[2] = (m_currentSpeed >> 8) & 0xFF;
	cdb[3] = m_currentSpeed & 0xFF;
	cdb[4] = (writeSpeed >> 8) & 0xFF;
	cdb[5] = writeSpeed & 0xFF;
	BYTE dummy[4] = {};
	SendSCSI(cdb, 12, dummy, 0, false);
}

bool ScsiDrive::GetActualSpeed(WORD& readSpeed, WORD& writeSpeed) {
	// Need header (8) + page 2A up to byte 19 (current write speed) + slack
	// for possible block descriptors.
	static constexpr int BUF_SIZE = 64;
	BYTE cdb[10] = { 0x5A, 0, 0x2A, 0, 0, 0, 0, 0, BUF_SIZE, 0 };
	BYTE buffer[BUF_SIZE] = {};

	if (!SendSCSI(cdb, 10, buffer, BUF_SIZE, true))
		return false;

	// MODE SENSE (10) header is 8 bytes; skip any block descriptors.
	int bdLen = (buffer[6] << 8) | buffer[7];
	int pageOff = 8 + bdLen;

	// Sanity: verify page 2A was returned and we have enough data.
	if (pageOff + 20 > BUF_SIZE || (buffer[pageOff] & 0x3F) != 0x2A)
		return false;

	BYTE* page = &buffer[pageOff];
	readSpeed  = (page[14] << 8) | page[15];   // Current read speed  (kB/s)
	writeSpeed = (page[18] << 8) | page[19];   // Current write speed (kB/s)

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

bool ScsiDrive::GetMediaProfile(WORD& profileCode, std::string& profileName) {
	// GET CONFIGURATION (0x46) — request current profile only
	BYTE cdb[10] = { 0x46, 0x01, 0, 0, 0, 0, 0, 0, 16, 0 };
	BYTE buf[16] = {};

	if (!SendSCSI(cdb, 10, buf, 16)) {
		profileCode = 0;
		profileName = "Unknown";
		return false;
	}

	// Current profile is at bytes 6-7 of the feature header
	profileCode = (static_cast<WORD>(buf[6]) << 8) | buf[7];

	switch (profileCode) {
	case 0x0008: profileName = "CD-ROM";  break;   // Factory-pressed CD
	case 0x0009: profileName = "CD-R";    break;   // Recordable CD
	case 0x000A: profileName = "CD-RW";   break;   // Rewritable CD
	case 0x0010: profileName = "DVD-ROM"; break;
	case 0x0011: profileName = "DVD-R";   break;
	case 0x0012: profileName = "DVD-RAM"; break;
	default:     profileName = "Unknown"; break;
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

bool ScsiDrive::RequestSenseProgress(BYTE& senseKey, BYTE& asc, BYTE& ascq, int& progressPercent) {
	progressPercent = -1;
	if (m_handle == INVALID_HANDLE_VALUE) return false;

	// REQUEST SENSE (0x03) — 18 bytes for fixed-format sense with SKS fields
	BYTE cdb[6] = { 0x03, 0x00, 0x00, 0x00, 18, 0x00 };
	BYTE sense[18] = {};

	if (!SendSCSI(cdb, sizeof(cdb), sense, sizeof(sense), true))
		return false;

	// Fixed-format sense: response code 0x70 or 0x71
	if ((sense[0] & 0x7F) != 0x70 && (sense[0] & 0x7F) != 0x71)
		return false;

	senseKey = sense[2] & 0x0F;
	asc = sense[12];
	ascq = sense[13];

	// Sense Key Specific Valid (SKSV) = bit 7 of byte 15
	// Progress indication in bytes 16-17 (big-endian, 0x0000–0xFFFF)
	if (sense[15] & 0x80) {
		WORD progress = (static_cast<WORD>(sense[16]) << 8) | sense[17];
		progressPercent = static_cast<int>(progress * 100UL / 0xFFFF);
	}

	return true;
}

bool ScsiDrive::SpinDown() {
	if (m_handle == INVALID_HANDLE_VALUE) return false;

	// START STOP UNIT (0x1B): Start=0, LoEj=0 → spin down without ejecting
	BYTE cdb[6] = {};
	cdb[0] = 0x1B;   // START STOP UNIT
	// cdb[4] bits: LoEj=0, Start=0 → stop the motor
	BYTE dummy[4] = {};
	return SendSCSI(cdb, 6, dummy, 0, false);
}