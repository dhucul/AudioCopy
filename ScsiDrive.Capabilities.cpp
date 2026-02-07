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

	// C2 + block error bits (byte 9 bits 2:1 = 10b → 0xFC)
	BYTE cdb1[12] = { SCSI_READ_CD, 0x04, 0, 0, 0, 0, 0, 0, 1, 0xFC, 0x00, 0 };
	std::vector<BYTE> buffer(SECTOR_WITH_C2_SIZE);
	if (SendSCSI(cdb1, 12, buffer.data(), SECTOR_WITH_C2_SIZE)) {
		m_c2Mode = C2Mode::ErrorPointers;
		return true;
	}

	// C2 error block (byte 9 bits 2:1 = 01b → 0xFA)
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

	BYTE cdb[12] = {};
	cdb[0] = SCSI_READ_CD;
	cdb[1] = 0x04;
	cdb[8] = 1;
	cdb[9] = 0x10;

	if (leadIn) {
		// Try reading LBA -150 (pre-gap area before track 1)
		DWORD negLBA = static_cast<DWORD>(-150);
		cdb[2] = (negLBA >> 24) & 0xFF;
		cdb[3] = (negLBA >> 16) & 0xFF;
		cdb[4] = (negLBA >> 8) & 0xFF;
		cdb[5] = negLBA & 0xFF;
	}
	else {
		// Try reading past the last session's lead-out (LBA from TOC + a small offset)
		// Use a high LBA that's typically beyond disc content
		DWORD leadOutLBA = 400000;  // ~89 min, beyond most CDs
		cdb[2] = (leadOutLBA >> 24) & 0xFF;
		cdb[3] = (leadOutLBA >> 16) & 0xFF;
		cdb[4] = (leadOutLBA >> 8) & 0xFF;
		cdb[5] = leadOutLBA & 0xFF;
	}

	return SendSCSI(cdb, 12, audio, AUDIO_SECTOR_SIZE);
}

// Helper: Query GET CONFIGURATION for a specific feature code
static bool HasFeature(ScsiDrive& drive, WORD featureCode,
	bool (ScsiDrive::* sendFn)(void*, BYTE, void*, DWORD, bool),
	std::vector<BYTE>& featureData)
{
	// GET CONFIGURATION (opcode 0x46), RT=10b (one feature)
	BYTE cdb[10] = {};
	cdb[0] = 0x46;
	cdb[1] = 0x02;  // RT=10b: return only the requested feature
	cdb[2] = (featureCode >> 8) & 0xFF;
	cdb[3] = featureCode & 0xFF;
	cdb[7] = 0x00;
	cdb[8] = 64;  // allocation length

	featureData.resize(64, 0);
	return (drive.*sendFn)(cdb, 10, featureData.data(), 64, true);
}

bool ScsiDrive::DetectCapabilities(DriveCapabilities& caps) {
	caps = DriveCapabilities{};

	// Reuse GetDriveInfo instead of duplicating INQUIRY logic
	if (!GetDriveInfo(caps.vendor, caps.model)) {
		return false;  // Can't even identify the drive
	}

	// Get firmware revision from INQUIRY (needs full 96 bytes)
	BYTE inqCdb[6] = { 0x12, 0, 0, 0, 96, 0 };
	std::vector<BYTE> inqBuffer(96, 0);
	if (SendSCSI(inqCdb, 6, inqBuffer.data(), 96)) {
		caps.firmware = std::string(reinterpret_cast<char*>(&inqBuffer[32]), 4);
		while (!caps.firmware.empty() && (caps.firmware.back() == ' ' || caps.firmware.back() == '\0'))
			caps.firmware.pop_back();
	}

	// VPD page 0x80: Unit Serial Number
	BYTE vpd80Cdb[6] = { 0x12, 0x01, 0x80, 0, 64, 0 };
	std::vector<BYTE> vpd80Buffer(64, 0);
	if (SendSCSI(vpd80Cdb, 6, vpd80Buffer.data(), 64)) {
		// Validate VPD page code to confirm drive actually supports this page
		if (vpd80Buffer[1] == 0x80) {
			int len = vpd80Buffer[3];
			if (len > 0 && len < 60) {
				caps.serialNumber = std::string(reinterpret_cast<char*>(&vpd80Buffer[4]), len);
				while (!caps.serialNumber.empty() && caps.serialNumber.back() == ' ')
					caps.serialNumber.pop_back();
			}
		}
	}

	// Parse Mode Page 2A (CD/DVD Capabilities and Mechanical Status)
	std::vector<BYTE> pageData;
	if (GetModePage2A(pageData)) {
		if (pageData.size() >= 2) {  // Fix: prevent unsigned underflow
			size_t offset = (pageData[0] == 0) ? 8 : 4;

			while (offset + 2 < pageData.size()) {
				if ((pageData[offset] & 0x3F) == 0x2A) {
					int pageLen = pageData[offset + 1];
					size_t pageEnd = offset + 2 + pageLen;

					// Fix: validate page fits within buffer
					if (pageEnd > pageData.size())
						break;

					BYTE* page = &pageData[offset];

					if (pageLen >= 4) {
						// Byte 2: Read capabilities
						caps.readsCDR = (page[2] & 0x01) != 0;
						caps.readsCDRW = (page[2] & 0x02) != 0;
						caps.readsDVD = (page[2] & 0x08) != 0;
						// Bit 6 is reserved; BD detected via GET CONFIGURATION

						// Byte 3: Write capabilities
						caps.writesCDR = (page[3] & 0x01) != 0;
						caps.writesCDRW = (page[3] & 0x02) != 0;
						caps.supportsTestWrite = (page[3] & 0x04) != 0;  // Test Write bit
						caps.writesDVD = (page[3] & 0x10) != 0;          // DVD-R
						caps.writesDVDRAM = (page[3] & 0x20) != 0;       // DVD-RAM
					}

					if (pageLen >= 5) {
						// Byte 4: General device capabilities
						caps.supportsDigitalAudioPlay = (page[4] & 0x01) != 0;  // Audio Play
						caps.supportsCompositeOutput = (page[4] & 0x02) != 0;   // Composite
						// Bits 2-3: Digital Port 1/2 (not tracked)
						// Bits 4-5: Mode 2 Form 1/2 (not tracked)
						caps.supportsMultiSession = (page[4] & 0x40) != 0;      // Multi-Session
						caps.supportsBufferUnderrunProtection = (page[4] & 0x80) != 0; // BUP
					}

					if (pageLen >= 6) {
						// Byte 5: CD-DA / subchannel capabilities
						caps.supportsCDDA = (page[5] & 0x01) != 0;              // CD-DA commands
						caps.supportsAccurateStream = (page[5] & 0x02) != 0;    // CD-DA Stream Accurate
						caps.supportsSubchannelRaw = (page[5] & 0x04) != 0;     // R-W Supported
						caps.supportsSubchannelQ = (page[5] & 0x08) != 0;       // R-W De-interleaved
						// Bit 4: C2 Pointers (checked via CheckC2Support())
						// Bit 5: ISRC, Bit 6: UPC, Bit 7: Bar Code (not tracked)
					}

					if (pageLen >= 7) {
						// Byte 6: Mechanical capabilities
						caps.supportsLockMedia = (page[6] & 0x01) != 0;
						caps.supportsEject = (page[6] & 0x08) != 0;
						caps.isChanger = (page[6] & 0x10) != 0;
						caps.loadingMechanism = (page[6] >> 5) & 0x07;
					}

					if (pageLen >= 8) {
						// Byte 7: Volume / changer capabilities
						caps.supportsSeparateVolume = (page[7] & 0x01) != 0;    // Separate Volume
						caps.supportsSeparateMute = (page[7] & 0x02) != 0;      // Separate Channel Mute
					}

					if (pageLen >= 15) {
						// Bytes 8-9: Max read speed (kB/s)
						caps.maxReadSpeedKB = (page[8] << 8) | page[9];
						// Bytes 14-15: Current read speed (kB/s)
						caps.currentReadSpeedKB = (page[14] << 8) | page[15];
					}

					if (pageLen >= 13) {
						// Bytes 12-13: Buffer size (kB)
						caps.bufferSizeKB = (page[12] << 8) | page[13];
					}

					if (pageLen >= 25) {
						// Bytes 24-25: Current write speed (kB/s) — MMC-3+
						caps.currentWriteSpeedKB = (page[24] << 8) | page[25];
					}

					// Write speed descriptors (MMC-3+): starting at byte 28
					// Bytes 26-27: total length (in bytes) of descriptor table
					if (pageLen >= 28) {
						int wsdTotalLen = (page[26] << 8) | page[27];
						int numWsd = wsdTotalLen / 4;  // Each descriptor is 4 bytes
						caps.maxWriteSpeedKB = 0;
						for (int w = 0; w < numWsd && (28 + w * 4 + 3) < pageLen + 2; w++) {
							int woff = 28 + w * 4;
							int wspeed = (page[woff + 2] << 8) | page[woff + 3];
							if (wspeed > 0) {
								caps.supportedWriteSpeeds.push_back(wspeed);
								if (wspeed > caps.maxWriteSpeedKB)
									caps.maxWriteSpeedKB = wspeed;
							}
						}
					}

					break;
				}
				offset += 2 + pageData[offset + 1];
			}
		}
	}

	// ----------------------------------------------------------------
	// GET CONFIGURATION - query write mode features (TAO, SAO, RAW)
	// ----------------------------------------------------------------
	// Feature 0x002D: CD Track at Once
	{
		std::vector<BYTE> feat;
		BYTE cdb[10] = { 0x46, 0x02, 0x00, 0x2D, 0, 0, 0, 0x00, 64, 0 };
		feat.resize(64, 0);
		if (SendSCSI(cdb, 10, feat.data(), 64)) {
			int dataLen = (feat[0] << 24) | (feat[1] << 16) | (feat[2] << 8) | feat[3];
			if (dataLen >= 12) {
				WORD code = (feat[8] << 8) | feat[9];
				if (code == 0x002D)
					caps.supportsWriteTAO = true;
			}
		}
	}

	// Feature 0x002E: CD Mastering (SAO/DAO)
	{
		std::vector<BYTE> feat;
		BYTE cdb[10] = { 0x46, 0x02, 0x00, 0x2E, 0, 0, 0, 0x00, 64, 0 };
		feat.resize(64, 0);
		if (SendSCSI(cdb, 10, feat.data(), 64)) {
			int dataLen = (feat[0] << 24) | (feat[1] << 16) | (feat[2] << 8) | feat[3];
			if (dataLen >= 12) {
				WORD code = (feat[8] << 8) | feat[9];
				if (code == 0x002E)
					caps.supportsWriteSAO = true;
			}
		}
	}

	// Feature 0x0001: Core Feature — also check for BD profile
	{
		std::vector<BYTE> feat;
		BYTE cdb[10] = { 0x46, 0x02, 0x00, 0x01, 0, 0, 0, 0x00, 64, 0 };
		feat.resize(64, 0);
		if (SendSCSI(cdb, 10, feat.data(), 64)) {
			WORD currentProfile = (feat[6] << 8) | feat[7];
			// BD-R (0x0041/0x0042) or BD-RE (0x0043)
			if (currentProfile >= 0x0040 && currentProfile <= 0x004F)
				caps.writesBD = true;
		}
	}

	// Feature 0x0107: CD-RW Media Write (Real-Time Streaming / BUP fallback)
	// Some drives report BUP here rather than in Mode Page 2A
	if (!caps.supportsBufferUnderrunProtection) {
		std::vector<BYTE> feat;
		BYTE cdb[10] = { 0x46, 0x02, 0x01, 0x07, 0, 0, 0, 0x00, 64, 0 };
		feat.resize(64, 0);
		if (SendSCSI(cdb, 10, feat.data(), 64)) {
			int dataLen = (feat[0] << 24) | (feat[1] << 16) | (feat[2] << 8) | feat[3];
			if (dataLen >= 12) {
				WORD code = (feat[8] << 8) | feat[9];
				if (code == 0x0107)
					caps.supportsBufferUnderrunProtection = true;
			}
		}
	}

	// ----------------------------------------------------------------
	// GET PERFORMANCE - query supported read speeds
	// ----------------------------------------------------------------
	static constexpr int MAX_PERF_DESCRIPTORS = 8;
	static constexpr int PERF_HEADER_SIZE = 8;
	static constexpr int PERF_DESC_SIZE = 16;
	static constexpr int PERF_BUFFER_SIZE = PERF_HEADER_SIZE + MAX_PERF_DESCRIPTORS * PERF_DESC_SIZE;

	// Type 0x03: Read speed descriptors
	BYTE perfCdb[12] = { 0xAC, 0x10, 0, 0, 0, 0, 0, 0, 0, 0, 0x03, 0 };
	perfCdb[9] = MAX_PERF_DESCRIPTORS;
	std::vector<BYTE> perfBuffer(PERF_BUFFER_SIZE, 0);
	if (SendSCSI(perfCdb, 12, perfBuffer.data(), PERF_BUFFER_SIZE)) {
		int dataLen = (perfBuffer[0] << 24) | (perfBuffer[1] << 16) |
			(perfBuffer[2] << 8) | perfBuffer[3];
		int numDescriptors = (dataLen - 4) / PERF_DESC_SIZE;
		if (numDescriptors > MAX_PERF_DESCRIPTORS)
			numDescriptors = MAX_PERF_DESCRIPTORS;
		for (int i = 0; i < numDescriptors; i++) {
			int off = PERF_HEADER_SIZE + i * PERF_DESC_SIZE;
			if (off + PERF_DESC_SIZE > static_cast<int>(perfBuffer.size()))
				break;
			int speed = (perfBuffer[off + 12] << 24) | (perfBuffer[off + 13] << 16) |
				(perfBuffer[off + 14] << 8) | perfBuffer[off + 15];
			if (speed > 0) {
				caps.supportedReadSpeeds.push_back(speed);
			}
		}
	}

	// Type 0x03 with write bit: Write speed descriptors
	BYTE wperfCdb[12] = { 0xAC, 0x10, 0, 0, 0, 0, 0, 0, 0, 0, 0x03, 0 };
	wperfCdb[1] = 0x14;  // Write=1, Tolerance bits remain 00
	wperfCdb[9] = MAX_PERF_DESCRIPTORS;
	std::vector<BYTE> wperfBuffer(PERF_BUFFER_SIZE, 0);
	if (SendSCSI(wperfCdb, 12, wperfBuffer.data(), PERF_BUFFER_SIZE)) {
		int dataLen = (wperfBuffer[0] << 24) | (wperfBuffer[1] << 16) |
			(wperfBuffer[2] << 8) | wperfBuffer[3];
		int numDescriptors = (dataLen - 4) / PERF_DESC_SIZE;
		if (numDescriptors > MAX_PERF_DESCRIPTORS)
			numDescriptors = MAX_PERF_DESCRIPTORS;
		for (int i = 0; i < numDescriptors; i++) {
			int off = PERF_HEADER_SIZE + i * PERF_DESC_SIZE;
			if (off + PERF_DESC_SIZE > static_cast<int>(wperfBuffer.size()))
				break;
			int speed = (wperfBuffer[off + 12] << 24) | (wperfBuffer[off + 13] << 16) |
				(wperfBuffer[off + 14] << 8) | wperfBuffer[off + 15];
			if (speed > 0) {
				caps.supportedWriteSpeeds.push_back(speed);
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