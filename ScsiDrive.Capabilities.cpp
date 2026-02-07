// ============================================================================
// ScsiDrive.Capabilities.cpp - Capability and feature detection
// ============================================================================
#include "ScsiDrive.h"
#include <vector>

bool ScsiDrive::CheckC2Support() {
	std::string vendor, model;
	GetDriveInfo(vendor, model);

	if (vendor.find("PLEXTOR") != std::string::npos) {
		BYTE cdb[12] = { 0xD8, 0, 0, 0, 0, 0, 0, 0, 1, 0x02, 0, 0 };
		std::vector<BYTE> buffer(SECTOR_WITH_C2_SIZE);
		if (SendSCSI(cdb, 12, buffer.data(), SECTOR_WITH_C2_SIZE)) {
			m_c2Mode = C2Mode::PlextorD8;
			return true;
		}
	}

	BYTE cdb1[12] = { SCSI_READ_CD, 0x00, 0, 0, 0, 0, 0, 0, 1, 0xF8, 0xD8, 0 };
	std::vector<BYTE> buffer(SECTOR_WITH_C2_SIZE);
	if (SendSCSI(cdb1, 12, buffer.data(), SECTOR_WITH_C2_SIZE)) {
		m_c2Mode = C2Mode::ErrorPointers;
		return true;
	}

	BYTE cdb2[12] = { SCSI_READ_CD, 0x04, 0, 0, 0, 0, 0, 0, 1, 0xFA, 0x00, 0 };
	if (SendSCSI(cdb2, 12, buffer.data(), SECTOR_WITH_C2_SIZE)) {
		m_c2Mode = C2Mode::ErrorBlock;
		return true;
	}

	m_c2Mode = C2Mode::NotSupported;
	return false;
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

bool ScsiDrive::GetModePage2A(std::vector<BYTE>& pageData) {
	BYTE cdb[10] = {};
	cdb[0] = 0x5A;
	cdb[2] = 0x2A;
	cdb[7] = 0x00;
	cdb[8] = 0xFF;

	pageData.resize(255, 0);
	if (!SendSCSI(cdb, 10, pageData.data(), 255)) {
		BYTE cdb6[6] = {};
		cdb6[0] = 0x1A;
		cdb6[2] = 0x2A;
		cdb6[4] = 0xFF;

		if (!SendSCSI(cdb6, 6, pageData.data(), 255)) {
			return false;
		}
	}
	return true;
}

bool ScsiDrive::TestOverread(bool leadIn) {
	BYTE audio[AUDIO_SECTOR_SIZE];
	DWORD testLBA = leadIn ? 0xFFFFFF00 : 0;

	if (leadIn) {
		BYTE cdb[12] = {};
		cdb[0] = SCSI_READ_CD;
		cdb[1] = 0x04;
		DWORD negLBA = static_cast<DWORD>(-150);
		cdb[2] = (negLBA >> 24) & 0xFF;
		cdb[3] = (negLBA >> 16) & 0xFF;
		cdb[4] = (negLBA >> 8) & 0xFF;
		cdb[5] = negLBA & 0xFF;
		cdb[8] = 1;
		cdb[9] = 0x10;
		return SendSCSI(cdb, 12, audio, AUDIO_SECTOR_SIZE);
	}
	else {
		return false;
	}
}

bool ScsiDrive::DetectCapabilities(DriveCapabilities& caps) {
	caps = DriveCapabilities{};

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

	std::vector<BYTE> pageData;
	if (GetModePage2A(pageData)) {
		size_t offset = (pageData[0] == 0) ? 8 : 4;

		while (offset < pageData.size() - 2) {
			if ((pageData[offset] & 0x3F) == 0x2A) {
				BYTE* page = &pageData[offset];
				int pageLen = page[1];

				if (pageLen >= 4) {
					caps.readsCDR = (page[2] & 0x01) != 0;
					caps.readsCDRW = (page[2] & 0x02) != 0;
					caps.readsDVD = (page[2] & 0x08) != 0;

					caps.writesCDR = (page[3] & 0x01) != 0;
					caps.writesCDRW = (page[3] & 0x02) != 0;
					caps.writesDVD = (page[3] & 0x10) != 0;

					caps.supportsCDDA = (page[4] & 0x01) != 0;
					caps.supportsAccurateStream = (page[4] & 0x02) != 0;
					caps.supportsSubchannelRaw = (page[4] & 0x04) != 0;
					caps.supportsSubchannelQ = (page[4] & 0x08) != 0;
					caps.supportsCDText = (page[4] & 0x40) != 0;
					caps.supportsDigitalAudioPlay = (page[4] & 0x80) != 0;

					caps.supportsSeparateVolume = (page[5] & 0x01) != 0;
					caps.supportsCompositeOutput = (page[5] & 0x02) != 0;
					caps.supportsSeparateMute = (page[5] & 0x08) != 0;
					caps.supportsMultiSession = (page[5] & 0x40) != 0;

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

	DWORD ret;
	caps.mediaPresent = DeviceIoControl(m_handle, IOCTL_STORAGE_CHECK_VERIFY,
		nullptr, 0, nullptr, 0, &ret, nullptr) != 0;

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