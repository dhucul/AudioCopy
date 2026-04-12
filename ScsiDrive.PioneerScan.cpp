// ============================================================================
// ScsiDrive.PioneerScan.cpp - Pioneer vendor quality scan implementation
//
// Protocol (derived from Pioneer firmware analysis):
//
//   Send scan request:  CDB  3B 02 E1 00 00 00 00 00 20 00  (WRITE BUFFER)
//   Read scan results:  CDB  3C 02 E1 00 00 00 00 00 20 00  (READ BUFFER)
//
// CD scan payload (32 bytes):
//   Byte 0-1:   FF 01          — enable scan mode
//   Byte 4-7:   LBA + 0x6000   — scan start address (big-endian)
//   Byte 8-11:  sector count   — number of sectors to scan
//   Byte 12-15: sector count   — duplicated
//
// Returned results (32 bytes):
//   rd_buf + 5:   E22 / C2 error count (big-endian 16-bit)
//   rd_buf + 13:  BLER / C1 error count (big-endian 16-bit)
//
// Two-phase model: send request (WRITE), then retrieve results (READ).
// Each poll covers one time-slice of ~75 sectors (1 second at 1x).
// ============================================================================
#include "ScsiDrive.h"
#include <iostream>
#include <iomanip>
#include <vector>
#include <cstring>
#include <thread>
#include <chrono>

// Pioneer scan state
static DWORD s_pioneerLBA = 0;
static DWORD s_pioneerEndLBA = 0;
static constexpr DWORD PIONEER_SCAN_BUFFER_SIZE = 32;
static constexpr DWORD PIONEER_CD_SECTORS_PER_SLICE = 75;   // 1 second at 1x
static constexpr DWORD PIONEER_LBA_OFFSET = 0x6000;

// Pioneer diagnostic dump counter
static int s_pioneerDiagDumps = 0;

// Build the 10-byte CDB common to both WRITE (0x3B) and READ (0x3C)
static void BuildPioneerCDB(BYTE* cdb, BYTE opcode) {
	memset(cdb, 0, 10);
	cdb[0] = opcode;       // 0x3B = WRITE BUFFER, 0x3C = READ BUFFER
	cdb[1] = 0x02;         // Mode / subcommand
	cdb[2] = 0xE1;         // Feature selector (Pioneer vendor-specific)
	cdb[8] = 0x20;         // Transfer length = 32 bytes
}

// Build a CD scan payload: 32 bytes sent via WRITE BUFFER
static void BuildCDScanPayload(BYTE* payload, DWORD lba, DWORD sectorCount) {
	memset(payload, 0, PIONEER_SCAN_BUFFER_SIZE);

	payload[0] = 0xFF;     // Enable scan mode
	payload[1] = 0x01;

	// LBA + offset (big-endian)
	DWORD addr = lba + PIONEER_LBA_OFFSET;
	payload[4] = static_cast<BYTE>((addr >> 24) & 0xFF);
	payload[5] = static_cast<BYTE>((addr >> 16) & 0xFF);
	payload[6] = static_cast<BYTE>((addr >> 8) & 0xFF);
	payload[7] = static_cast<BYTE>(addr & 0xFF);

	// Sector count (duplicated in two positions)
	payload[8] = static_cast<BYTE>((sectorCount >> 24) & 0xFF);
	payload[9] = static_cast<BYTE>((sectorCount >> 16) & 0xFF);
	payload[10] = static_cast<BYTE>((sectorCount >> 8) & 0xFF);
	payload[11] = static_cast<BYTE>(sectorCount & 0xFF);

	payload[12] = static_cast<BYTE>((sectorCount >> 24) & 0xFF);
	payload[13] = static_cast<BYTE>((sectorCount >> 16) & 0xFF);
	payload[14] = static_cast<BYTE>((sectorCount >> 8) & 0xFF);
	payload[15] = static_cast<BYTE>(sectorCount & 0xFF);
}

bool ScsiDrive::SupportsPioneerScan() {
	if (m_pioneerScanProbed >= 0)
		return m_pioneerScanProbed == 1;

	std::string vendor, model;
	GetDriveInfo(vendor, model);

	// Only probe on Pioneer drives
	bool isPioneer = false;
	for (size_t i = 0; i < vendor.size(); i++) {
		if (toupper(static_cast<unsigned char>(vendor[i])) == 'P') {
			if (vendor.size() - i >= 7 &&
				_strnicmp(vendor.c_str() + i, "PIONEER", 7) == 0) {
				isPioneer = true;
				break;
			}
		}
	}

	if (!isPioneer) {
		m_pioneerScanProbed = 0;
		return false;
	}

	std::cout << "  [PioneerScan] Probing scan support on "
		<< vendor << " " << model << "...\n" << std::flush;

	// Send a minimal WRITE BUFFER to see if the drive accepts it
	BYTE cdb[10] = {};
	BuildPioneerCDB(cdb, 0x3B);

	BYTE payload[PIONEER_SCAN_BUFFER_SIZE] = {};
	payload[0] = 0xFF;
	payload[1] = 0x01;

	BYTE sk = 0, asc = 0, ascq = 0;
	bool ok = SendSCSIWithSense(cdb, 10, payload, PIONEER_SCAN_BUFFER_SIZE,
		&sk, &asc, &ascq, /*dataIn=*/false);

	char dbg[256];
	snprintf(dbg, sizeof(dbg),
		"PioneerScan: 0x3B probe ok=%d sk=0x%02X asc=0x%02X ascq=0x%02X\n",
		ok, sk, asc, ascq);
	OutputDebugStringA(dbg);

	if (!ok && sk > 0x01) {
		// WRITE BUFFER rejected outright
		if (asc == 0x20) {
			std::cout << "  [PioneerScan] Drive does not recognize opcode 0x3B (ASC=0x20)\n" << std::flush;
		}
		else {
			std::cout << "  [PioneerScan] Probe rejected (SK=0x"
				<< std::hex << static_cast<int>(sk)
				<< " ASC=0x" << static_cast<int>(asc)
				<< " ASCQ=0x" << static_cast<int>(ascq)
				<< std::dec << ")\n" << std::flush;
		}
		m_pioneerScanProbed = 0;
		return false;
	}

	// WRITE BUFFER accepted — now verify the drive actually entered
	// measurement mode by reading back the scan state.  A drive that
	// truly supports the scan protocol returns a non-zero status in
	// byte 0 of the READ BUFFER response (typically 0xFF when active).
	// Drives that accept the command but don't implement measurement
	// mode return 0x04 or 0x00 — "acknowledged but idle."
	std::this_thread::sleep_for(std::chrono::milliseconds(500));

	BYTE readCdb[10] = {};
	BuildPioneerCDB(readCdb, 0x3C);

	BYTE rdBuf[PIONEER_SCAN_BUFFER_SIZE] = {};
	sk = 0; asc = 0; ascq = 0;
	bool readOk = SendSCSIWithSense(readCdb, 10, rdBuf, PIONEER_SCAN_BUFFER_SIZE,
		&sk, &asc, &ascq);

	snprintf(dbg, sizeof(dbg),
		"PioneerScan: 0x3C verify ok=%d sk=0x%02X byte0=0x%02X\n",
		readOk, sk, rdBuf[0]);
	OutputDebugStringA(dbg);

	// Clean up — don't leave the drive in scan mode after probing
	PioneerScanStop();

	// Check if the response indicates an active scan state.
	// Byte 0 == 0x04 or 0x00 means the drive is idle — it accepted the
	// command but did not enter measurement mode.  Only byte 0 >= 0x80
	// (or specifically 0xFF matching our FF 01 enable header) indicates
	// the drive is actually scanning.
	if (readOk && rdBuf[0] >= 0x80) {
		std::cout << "  [PioneerScan] Drive supports 0x3B/0x3C quality scan"
			<< " (status=0x" << std::hex << static_cast<int>(rdBuf[0])
			<< std::dec << ")\n" << std::flush;
		m_pioneerScanProbed = 1;
		return true;
	}

	// Drive accepted the command but returned an idle status — the
	// measurement mode is not implemented in this firmware.
	std::cout << "  [PioneerScan] Drive accepts 0x3B/0x3C but does not enter"
		<< " measurement mode (status=0x" << std::hex
		<< static_cast<int>(rdBuf[0]) << std::dec << ")\n" << std::flush;
	m_pioneerScanProbed = 0;
	return false;
}

bool ScsiDrive::PioneerScanStart(DWORD startLBA, DWORD endLBA) {
	s_pioneerLBA = startLBA;
	s_pioneerEndLBA = endLBA;

	std::cout << "  [PioneerScan] Starting CD scan from LBA "
		<< startLBA << " to " << endLBA << "\n" << std::flush;

	// Issue the first WRITE BUFFER with the starting position
	BYTE cdb[10] = {};
	BuildPioneerCDB(cdb, 0x3B);

	BYTE payload[PIONEER_SCAN_BUFFER_SIZE] = {};
	BuildCDScanPayload(payload, startLBA, PIONEER_CD_SECTORS_PER_SLICE);

	BYTE sk = 0, asc = 0, ascq = 0;
	bool ok = SendSCSIWithSense(cdb, 10, payload, PIONEER_SCAN_BUFFER_SIZE,
		&sk, &asc, &ascq, /*dataIn=*/false);

	if (!ok && sk > 0x01) {
		std::cout << "  [PioneerScan] Start command failed (SK=0x"
			<< std::hex << static_cast<int>(sk) << std::dec << ")\n" << std::flush;
		return false;
	}

	s_pioneerDiagDumps = 5;

	return true;
}

bool ScsiDrive::PioneerScanPoll(int& c1, int& c2, int& cu,
	DWORD& currentLBA, bool& scanDone) {

	// Phase 1: READ BUFFER — retrieve results of the last scan slice
	BYTE readCdb[10] = {};
	BuildPioneerCDB(readCdb, 0x3C);

	BYTE rdBuf[PIONEER_SCAN_BUFFER_SIZE] = {};
	BYTE sk = 0, asc = 0, ascq = 0;

	bool ok = SendSCSIWithSense(readCdb, 10, rdBuf, PIONEER_SCAN_BUFFER_SIZE,
		&sk, &asc, &ascq);

	if (!ok && sk > 0x01) {
		scanDone = true;
		return false;
	}

	// Extract error counts from the response buffer
	// BLER (C1) is at offset 13 (big-endian 16-bit)
	c1 = (static_cast<int>(rdBuf[13]) << 8) | rdBuf[14];

	// E22 (C2) is at offset 5 (big-endian 16-bit)
	c2 = (static_cast<int>(rdBuf[5]) << 8) | rdBuf[6];

	// Pioneer doesn't report CU separately in CD mode
	cu = 0;

	currentLBA = s_pioneerLBA;

	// Check if we've reached the end
	if (s_pioneerLBA >= s_pioneerEndLBA) {
		scanDone = true;
		return true;
	}

	scanDone = false;

	// Advance LBA for next slice before issuing the next WRITE
	s_pioneerLBA += PIONEER_CD_SECTORS_PER_SLICE;

	// Phase 2: WRITE BUFFER — send next scan slice request
	BYTE writeCdb[10] = {};
	BuildPioneerCDB(writeCdb, 0x3B);

	BYTE payload[PIONEER_SCAN_BUFFER_SIZE] = {};
	BuildCDScanPayload(payload, s_pioneerLBA, PIONEER_CD_SECTORS_PER_SLICE);

	ok = SendSCSIWithSense(writeCdb, 10, payload, PIONEER_SCAN_BUFFER_SIZE,
		&sk, &asc, &ascq, /*dataIn=*/false);

	if (!ok && sk > 0x01) {
		// Can't continue scanning, but we did get valid data this round
		scanDone = true;
	}

	if (s_pioneerDiagDumps > 0) {
		std::cout << "\n  [PioneerScan poll] ok=" << (ok ? "true" : "false")
			<< " SK=0x" << std::hex << std::setfill('0') << std::setw(2)
			<< static_cast<int>(sk) << std::dec << std::setfill(' ') << "\n  ";
		for (DWORD i = 0; i < PIONEER_SCAN_BUFFER_SIZE; i++)
			std::cout << std::hex << std::setfill('0') << std::setw(2)
			<< static_cast<int>(rdBuf[i]) << " ";
		std::cout << std::dec << std::setfill(' ') << "\n" << std::flush;
		s_pioneerDiagDumps--;
	}

	return true;
}

bool ScsiDrive::PioneerScanStop() {
	// Send a zero payload to terminate the scan session
	BYTE cdb[10] = {};
	BuildPioneerCDB(cdb, 0x3B);

	BYTE payload[PIONEER_SCAN_BUFFER_SIZE] = {};
	// payload is all zeros — no FF 01 header = stop

	BYTE sk = 0, asc = 0, ascq = 0;
	SendSCSIWithSense(cdb, 10, payload, PIONEER_SCAN_BUFFER_SIZE,
		&sk, &asc, &ascq, /*dataIn=*/false);

	s_pioneerLBA = 0;
	s_pioneerEndLBA = 0;
	return true;
}