// ============================================================================
// Constants.h - CD sector geometry and SCSI command constants
// ============================================================================
#pragma once

#include <windows.h>
#include <cstdint>

// ── CD sector byte-layout constants ─────────────────────────────────────────
// A Red Book audio CD stores 2352 bytes of PCM audio per sector (16-bit
// stereo at 44100 Hz).  Subchannel data (P–W) adds 96 bytes.  The drive can
// optionally return C2 error pointers (296 bytes) alongside the audio.
constexpr int AUDIO_SECTOR_SIZE = 2352;       // Raw 16-bit stereo PCM per sector
constexpr int SUBCHANNEL_SIZE = 96;           // P–W subchannel block
constexpr int RAW_SECTOR_SIZE = 2448;         // Audio (2352) + subchannel (96)
constexpr int C2_ERROR_SIZE = 296;            // C2 error pointer / flag block
constexpr int SECTOR_WITH_C2_SIZE = 2648;     // Audio (2352) + C2 (296)
constexpr int FULL_SECTOR_WITH_C2 = 2744;     // Audio (2352) + subchannel (96) + C2 (296)

// ── Drive speed multipliers ─────────────────────────────────────────────────
// CD 1× = 176 KB/s data throughput.  0xFFFF is the MMC "maximum available" sentinel.
constexpr WORD CD_SPEED_1X = 176;             // 1× CD read speed in KB/s
constexpr WORD CD_SPEED_MAX = 0xFFFF;         // Ask drive for maximum speed

// ── SCSI MMC command opcodes ────────────────────────────────────────────────
// Only the two opcodes needed for raw audio extraction are declared here.
constexpr BYTE SCSI_READ_CD = 0xBE;           // READ CD – raw sector read
constexpr BYTE SCSI_SET_CD_SPEED = 0xBB;      // SET CD SPEED – spindle control

// ── Logging destination bitmask ─────────────────────────────────────────────
// Controls where diagnostic / progress messages are routed.
enum class LogOutput {
	None = 0,      // Suppress all output
	Console = 1,   // Write to stdout only
	File = 2,      // Write to a log file only
	Both = 3       // Write to stdout AND log file
};

// ── Shared subchannel utility functions ─────────────────────────────────────
// Used by both ScsiDrive (low-level reads) and AudioCDCopier (verification).
// Defined inline to avoid duplicate symbols across translation units.

// BCD-to-binary conversion.  CD subchannel data stores track, index, and MSF
// values in Binary-Coded Decimal.
inline BYTE BcdToBin(BYTE bcd) {
	return ((bcd >> 4) & 0x0F) * 10 + (bcd & 0x0F);
}

// CRC-16-CCITT for Q subchannel (polynomial 0x1021).  Validates Q-channel
// integrity: bytes 0-9 are checked against the CRC stored in bytes 10-11.
inline uint16_t SubchannelCRC16(const BYTE* data, int len) {
	uint16_t crc = 0;
	for (int i = 0; i < len; i++) {
		crc ^= static_cast<uint16_t>(data[i]) << 8;
		for (int bit = 0; bit < 8; bit++) {
			crc = (crc & 0x8000) ? (crc << 1) ^ 0x1021 : crc << 1;
		}
	}
	return crc;
}