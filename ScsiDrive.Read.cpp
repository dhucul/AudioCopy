// ============================================================================
// ScsiDrive.Read.cpp - SCSI sector reading and C2 handling
// ============================================================================
#include "ScsiDrive.h"
#include <climits>
#include <vector>

// C2 error pointer data is always 294 bytes.  In ErrorPointers mode the drive
// returns 296 bytes, but bytes 294-295 are C1/C2 *block error statistics* from
// the hardware ECC decoder — NOT error pointers.  C1 block errors are routine
// even on perfect discs and must never be counted as C2 errors.
constexpr int C2_POINTER_BYTES = 294;

bool ScsiDrive::ParseRawSubchannel(const BYTE* sub, int& qTrack, int& qIndex) {
	BYTE qchannel[12] = {};
	for (int i = 0; i < 96; i++) {
		int byteIdx = i / 8;
		int bitIdx = 7 - (i % 8);
		if (sub[i] & 0x40) {
			qchannel[byteIdx] |= (1 << bitIdx);
		}
	}

	// Validate CRC-16 (bytes 0-9 checked against bytes 10-11)
	uint16_t calcCrc = SubchannelCRC16(qchannel, 10);
	uint16_t storedCrc = (static_cast<uint16_t>(qchannel[10]) << 8) | qchannel[11];
	if (calcCrc != storedCrc) {
		return false;  // CRC mismatch — data is unreliable
	}

	// Only ADR=1 frames carry track/index position data
	BYTE adr = qchannel[0] & 0x0F;
	if (adr != 1) {
		return false;  // Not a position frame (MCN or ISRC)
	}

	// Q subchannel stores track/index in BCD
	qTrack = BcdToBin(qchannel[1]);
	qIndex = BcdToBin(qchannel[2]);
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

// FIX A: Delegate to ReadSectorWithC2Ex so m_c2Mode is respected everywhere.
// This fixes RunDiscRotScan, RunSpeedComparisonTest, CheckLeadAreas,
// GenerateSurfaceMap, and ScanDiscForC2Errors dual-speed validation.
bool ScsiDrive::ReadSectorWithC2(DWORD lba, BYTE* audio, BYTE* subchannel, int& c2Errors) {
	C2ReadOptions opts;
	return ReadSectorWithC2Ex(lba, audio, subchannel, c2Errors, nullptr, opts);
}

bool ScsiDrive::ReadSectorWithC2Ex(DWORD lba, BYTE* audio, BYTE* subchannel,
	int& c2Errors, BYTE* c2Raw, const C2ReadOptions& options,
	BYTE* outSenseKey, BYTE* outASC, BYTE* outASCQ,
	int* outC1BlockErrors, int* outC2BlockErrors) {

	if (options.multiPass && options.passCount > 1) {
		return ReadSectorWithC2ExMultiPass(lba, audio, subchannel, c2Errors, c2Raw, options,
			outSenseKey, outASC, outASCQ);
	}

	if (m_c2Mode == C2Mode::PlextorD8) {
		return PlextorReadC2(lba, audio, c2Errors, c2Raw, options.countBytes,
			outSenseKey, outASC, outASCQ,
			outC1BlockErrors, outC2BlockErrors);
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

	bool useErrorBlock = false;
	BYTE senseKey = 0, asc = 0, ascq = 0;

	if (m_c2Mode == C2Mode::ErrorBlock) {
		cdb[9] = 0xF8 | 0x02;
		cdb[10] = subchannel ? 0x01 : 0x00;
		useErrorBlock = true;
		bool ok = SendSCSIWithSense(cdb, 12, buffer.data(), bufferSize, &senseKey, &asc, &ascq);
		if (!ok && senseKey != 0x01) {
			if (outSenseKey) *outSenseKey = senseKey;
			if (outASC) *outASC = asc;
			if (outASCQ) *outASCQ = ascq;
			return false;
		}
	}
	else {
		cdb[9] = 0xF8 | 0x04;
		cdb[10] = subchannel ? 0x01 : 0x00;

		bool ok = SendSCSIWithSense(cdb, 12, buffer.data(), bufferSize, &senseKey, &asc, &ascq);
		if (!ok && senseKey != 0x01) {
			// First mode failed — try fallback
			cdb[9] = 0xF8 | 0x02;
			cdb[10] = subchannel ? 0x01 : 0x00;

			ok = SendSCSIWithSense(cdb, 12, buffer.data(), bufferSize, &senseKey, &asc, &ascq);
			if (!ok && senseKey != 0x01) {
				if (outSenseKey) *outSenseKey = senseKey;
				if (outASC) *outASC = asc;
				if (outASCQ) *outASCQ = ascq;
				return false;
			}
			useErrorBlock = true;
		}
	}

	memcpy(audio, buffer.data(), AUDIO_SECTOR_SIZE);

	c2Errors = 0;
	const BYTE* c2Data = buffer.data() + AUDIO_SECTOR_SIZE;

	// FIX: Only count the 294 actual C2 error pointer bytes.  Bytes 294-295
	// in ErrorPointers mode are C1/C2 block error statistics — C1 counts are
	// routinely non-zero on perfect discs and were causing false positives.
	for (int i = 0; i < C2_POINTER_BYTES; i++) {
		if (options.countBytes) {
			if (c2Data[i] != 0) c2Errors++;
		}
		else {
			BYTE b = c2Data[i];
			while (b) { c2Errors += b & 1; b >>= 1; }
		}
	}

	if (outSenseKey) *outSenseKey = senseKey;
	if (outASC) *outASC = asc;
	if (outASCQ) *outASCQ = ascq;

	if (c2Raw) {
		memset(c2Raw, 0, C2_ERROR_SIZE);
		memcpy(c2Raw, c2Data, C2_POINTER_BYTES);
	}

	// Extract C1/C2 block error statistics from bytes 294-295.
	// Only valid in ErrorPointers mode — ErrorBlock has a different layout.
	if (!useErrorBlock) {
		if (outC1BlockErrors) *outC1BlockErrors = static_cast<int>(c2Data[294]);
		if (outC2BlockErrors) *outC2BlockErrors = static_cast<int>(c2Data[295]);

		// Cross-check: the drive's CIRC decoder reports its own C2 block error
		// count in byte 295.  If it says zero but the pointer bitmap produced a
		// non-zero count, the bitmap contains drive-specific padding/status bytes
		// rather than real error pointers.  Trust the hardware counter.
		if (c2Errors > 0 && c2Data[295] == 0 && senseKey == 0x00) {
			c2Errors = 0;
			if (c2Raw) {
				memset(c2Raw, 0, C2_POINTER_BYTES);
			}
		}
	}
	else {
		if (outC1BlockErrors) *outC1BlockErrors = 0;
		if (outC2BlockErrors) *outC2BlockErrors = 0;
	}

	if (subchannel) {
		memcpy(subchannel, buffer.data() + AUDIO_SECTOR_SIZE + C2_ERROR_SIZE, SUBCHANNEL_SIZE);
	}

	return true;
}

// FIX B: Cache defeat between multi-pass reads
bool ScsiDrive::ReadSectorWithC2ExMultiPass(DWORD lba, BYTE* audio, BYTE* subchannel,
	int& c2Errors, BYTE* c2Raw, const C2ReadOptions& options,
	BYTE* outSenseKey, BYTE* outASC, BYTE* outASCQ) {

	std::vector<BYTE> bestAudio(AUDIO_SECTOR_SIZE);
	std::vector<BYTE> aggregatedC2(C2_ERROR_SIZE, 0);
	int minPassErrors = INT_MAX;
	BYTE worstSenseKey = 0x00;
	BYTE worstASC = 0x00;
	BYTE worstASCQ = 0x00;

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
		BYTE passSenseKey = 0, passASC = 0, passASCQ = 0;

		C2ReadOptions singleOpts;
		singleOpts.multiPass = false;
		singleOpts.passCount = 1;
		singleOpts.defeatCache = false;
		singleOpts.countBytes = options.countBytes;

		if (!ReadSectorWithC2Ex(lba, passAudio, subchannel, passErrors, passC2, singleOpts,
			&passSenseKey, &passASC, &passASCQ)) {
			if (outSenseKey) *outSenseKey = passSenseKey;
			if (outASC) *outASC = passASC;
			if (outASCQ) *outASCQ = passASCQ;
			return false;
		}

		// Keep the worst sense key across passes (0x03 > 0x01 > 0x00)
		if (passSenseKey > worstSenseKey) {
			worstSenseKey = passSenseKey;
			worstASC = passASC;
			worstASCQ = passASCQ;
		}

		// Only merge the actual C2 pointer bytes, not the block error stats
		for (int i = 0; i < C2_POINTER_BYTES; i++) {
			aggregatedC2[i] |= passC2[i];
		}

		if (passErrors < minPassErrors) {
			memcpy(bestAudio.data(), passAudio, AUDIO_SECTOR_SIZE);
			minPassErrors = passErrors;
		}
	}

	// FIX: Recount over 294 pointer bytes only — excludes block error stats
	c2Errors = 0;
	for (int i = 0; i < C2_POINTER_BYTES; i++) {
		if (options.countBytes) {
			// PlexTools-style: in ErrorPointers mode, 0xFF means "no error
			// sample pointer" — only non-0xFF bytes indicate actual errors.
			if (aggregatedC2[i] != 0 && aggregatedC2[i] != 0xFF) c2Errors++;
		}
		else {
			BYTE b = aggregatedC2[i];
			while (b) { c2Errors += b & 1; b >>= 1; }
		}
	}

	if (outSenseKey) *outSenseKey = worstSenseKey;
	if (outASC) *outASC = worstASC;
	if (outASCQ) *outASCQ = worstASCQ;

	memcpy(audio, bestAudio.data(), AUDIO_SECTOR_SIZE);
	if (c2Raw) memcpy(c2Raw, aggregatedC2.data(), C2_POINTER_BYTES);

	return true;
}

bool ScsiDrive::PlextorReadC2(DWORD lba, BYTE* audio, int& c2Errors, BYTE* c2Raw, bool countBytes,
	BYTE* outSenseKey, BYTE* outASC, BYTE* outASCQ,
	int* outC1BlockErrors, int* outC2BlockErrors) {
	BYTE cdb[12] = {};
	std::vector<BYTE> buffer(SECTOR_WITH_C2_SIZE);

	cdb[0] = 0xD8;
	cdb[2] = (lba >> 24) & 0xFF;
	cdb[3] = (lba >> 16) & 0xFF;
	cdb[4] = (lba >> 8) & 0xFF;
	cdb[5] = lba & 0xFF;
	cdb[8] = 1;
	cdb[9] = 0x02;

	BYTE senseKey = 0, asc = 0, ascq = 0;
	bool ok = SendSCSIWithSense(cdb, 12, buffer.data(), SECTOR_WITH_C2_SIZE,
		&senseKey, &asc, &ascq);

	if (outSenseKey) *outSenseKey = senseKey;
	if (outASC) *outASC = asc;
	if (outASCQ) *outASCQ = ascq;

	// Accept No Sense (0x00) and Recovered Error (0x01) — many Plextor
	// drives return CHECK CONDITION with sense key 0x00 on vendor D8 reads.
	// The data buffer is valid in both cases.
	if (!ok && senseKey > 0x01) {
		return false;
	}

	memcpy(audio, buffer.data(), AUDIO_SECTOR_SIZE);

	// FIX: Count only 294 pointer bytes — Plextor D8 also returns block
	// error stats in the trailing bytes that must not inflate the count.
	c2Errors = 0;
	const BYTE* c2Data = buffer.data() + AUDIO_SECTOR_SIZE;
	for (int i = 0; i < C2_POINTER_BYTES; i++) {
		if (countBytes) {
			if (c2Data[i] != 0) c2Errors++;
		}
		else {
			BYTE b = c2Data[i];
			while (b) { c2Errors += b & 1; b >>= 1; }
		}
	}

	if (c2Raw) {
		memcpy(c2Raw, c2Data, C2_POINTER_BYTES);
	}

	// Plextor D8 returns C1/C2 block error statistics in bytes 294-295
	// of the 296-byte C2 region.  These are hardware ECC decoder counts
	// per sector — the true BLER values that PlexTools reports.
	if (outC1BlockErrors) *outC1BlockErrors = static_cast<int>(c2Data[294]);
	if (outC2BlockErrors) *outC2BlockErrors = static_cast<int>(c2Data[295]);

	return true;
}

// FIX C & D: Restore missing functions (unresolved externals)

bool ScsiDrive::ValidateC2Accuracy(DWORD testLBA) {
	constexpr int NUM_READS = 4;
	constexpr int SPEEDS[] = { 4, 8, 16, 0 }; // 0 = max
	constexpr DWORD CACHE_DEFEAT_DISTANCE = 5000;

	std::vector<BYTE> audio(AUDIO_SECTOR_SIZE);
	std::vector<BYTE> c2First(C2_ERROR_SIZE);
	std::vector<BYTE> c2Current(C2_ERROR_SIZE);

	C2ReadOptions opts;
	opts.countBytes = false;

	// PHASE 1: Verify this is a clean sector (no C2 errors at baseline speed)
	SetSpeed(8); // Use medium speed for initial scan
	int preTestErrors = 0;
	if (!ReadSectorWithC2Ex(testLBA, audio.data(), nullptr, preTestErrors, nullptr, opts)) {
		SetSpeed(0);
		return false; // Read failure
	}

	// If sector has C2 errors, we can't validate C2 accuracy here
	// (errors might be real, so variation is expected)
	if (preTestErrors > 0) {
		SetSpeed(0);
		return true; // PASS - can't disprove C2 accuracy on error-containing sectors
	}

	// PHASE 2: Now verify the sector stays clean at all speeds
	for (int i = 0; i < NUM_READS; i++) {
		SetSpeed(SPEEDS[i]);

		// Cache defeat between reads
		DWORD farLBA = (testLBA > CACHE_DEFEAT_DISTANCE)
			? testLBA - CACHE_DEFEAT_DISTANCE
			: testLBA + CACHE_DEFEAT_DISTANCE;
		ReadSectorAudioOnly(farLBA, audio.data());
		Sleep(10);

		int c2Errors = 0;
		if (!ReadSectorWithC2Ex(testLBA, audio.data(), nullptr, c2Errors, nullptr, opts)) {
			SetSpeed(0);
			return false; // Read failure
		}

		// If a previously-clean sector now shows C2 errors, the reporting is unreliable
		if (c2Errors > 0) {
			SetSpeed(0);
			return false; // FAIL - phantom errors appeared
		}
	}

	SetSpeed(0);
	return true; // PASS - sector stayed clean at all speeds
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

bool ScsiDrive::ReadSectorQRaw(DWORD lba, int& qTrack, int& qIndex) {
	BYTE cdb[12] = {};
	BYTE buffer[RAW_SECTOR_SIZE];

	cdb[0] = SCSI_READ_CD;
	cdb[1] = 0x04;
	cdb[2] = (lba >> 24) & 0xFF;
	cdb[3] = (lba >> 16) & 0xFF;
	cdb[4] = (lba >> 8) & 0xFF;
	cdb[5] = lba & 0xFF;
	cdb[8] = 1;
	cdb[9] = 0xF8;
	cdb[10] = 0x01;  // Raw subchannel

	// Retry once on CRC failure — transient subchannel errors are common
	for (int attempt = 0; attempt < 2; attempt++) {
		if (!SendSCSI(cdb, 12, buffer, RAW_SECTOR_SIZE)) return false;
		if (ParseRawSubchannel(buffer + AUDIO_SECTOR_SIZE, qTrack, qIndex)) return true;
	}
	return false;
}

// Single-read Q subchannel helper (raw with formatted fallback, no voting)
bool ScsiDrive::ReadSectorQSingle(DWORD lba, int& qTrack, int& qIndex) {
	if (ReadSectorQRaw(lba, qTrack, qIndex)) {
		return true;
	}

	// Fallback: formatted Q subchannel
	BYTE cdb[12] = {};
	BYTE buffer[AUDIO_SECTOR_SIZE + 16];

	cdb[0] = SCSI_READ_CD;
	cdb[1] = 0x04;
	cdb[2] = (lba >> 24) & 0xFF;
	cdb[3] = (lba >> 16) & 0xFF;
	cdb[4] = (lba >> 8) & 0xFF;
	cdb[5] = lba & 0xFF;
	cdb[8] = 1;
	cdb[9] = 0xF8;
	cdb[10] = 0x02;  // Formatted Q subchannel

	if (!SendSCSI(cdb, 12, buffer, AUDIO_SECTOR_SIZE + 16)) return false;

	const BYTE* qData = buffer + AUDIO_SECTOR_SIZE;

	// Validate ADR mode — only mode 1 carries position data
	BYTE adr = qData[0] & 0x0F;
	if (adr != 1) return false;

	// Validate BCD ranges before conversion
	if ((qData[1] & 0xF0) > 0x90 || (qData[1] & 0x0F) > 0x09) return false;
	if ((qData[2] & 0xF0) > 0x90 || (qData[2] & 0x0F) > 0x09) return false;

	qTrack = BcdToBin(qData[1]);
	qIndex = BcdToBin(qData[2]);
	return true;
}

// Majority-voting Q subchannel read — performs 3 single reads and returns
// the (track, index) pair that at least 2 of 3 reads agree on.  This defeats
// stale subchannel data that many drives return at index transition boundaries.
bool ScsiDrive::ReadSectorQ(DWORD lba, int& qTrack, int& qIndex) {
	constexpr int ROUNDS = 3;
	constexpr int MAJORITY = 2;

	struct QResult { int track; int index; };
	QResult results[ROUNDS];
	int validCount = 0;

	for (int round = 0; round < ROUNDS; round++) {
		int t = 0, idx = -1;
		if (ReadSectorQSingle(lba, t, idx)) {
			results[validCount++] = { t, idx };
		}
	}

	if (validCount == 0) return false;

	// Find a (track, index) pair that appears >= MAJORITY times
	for (int i = 0; i < validCount; i++) {
		int count = 0;
		for (int j = 0; j < validCount; j++) {
			if (results[j].track == results[i].track &&
				results[j].index == results[i].index) {
				count++;
			}
		}
		if (count >= MAJORITY) {
			qTrack = results[i].track;
			qIndex = results[i].index;
			return true;
		}
	}

	// No majority — return first valid result as best-effort
	qTrack = results[0].track;
	qIndex = results[0].index;
	return true;
}

// Adaptive Q subchannel read — uses a single read for sectors deep within a
// track and falls back to majority voting only near index transition points
// (pregap/start boundaries) where drives commonly return stale data.
bool ScsiDrive::ReadSectorQAdaptive(DWORD lba, int& qTrack, int& qIndex,
	DWORD pregapLBA, DWORD startLBA) {
	constexpr DWORD TRANSITION_MARGIN = 75;  // +/- 1 second (75 sectors) around boundaries

	bool nearTransition = false;
	if (lba >= pregapLBA && lba < pregapLBA + TRANSITION_MARGIN) nearTransition = true;
	if (lba >= startLBA && lba < startLBA + TRANSITION_MARGIN)  nearTransition = true;
	if (pregapLBA > TRANSITION_MARGIN && lba >= pregapLBA - TRANSITION_MARGIN && lba < pregapLBA)
		nearTransition = true;

	if (nearTransition) {
		return ReadSectorQ(lba, qTrack, qIndex);  // Full 3-read majority voting
	}

	// Single read for mid-track sectors; fall back to voting on failure
	if (ReadSectorQSingle(lba, qTrack, qIndex)) {
		return true;
	}
	return ReadSectorQ(lba, qTrack, qIndex);
}

// A C2 error is likely recoverable if:
// 1. Re-read at a different speed produces c2Errors == 0 for the same LBA
// 2. Multiple reads return identical audio bytes (matching hash)
// 3. SCSI sense key == 0x01 (Recovered Error)
//
// A C2 error is likely unrecoverable if:
// 1. Every re-read still shows C2 errors
// 2. Audio data differs across reads (hash mismatch)
// 3. SCSI sense key == 0x03 (Medium Error)

// Add a new method for reading multiple consecutive sectors at once
bool ScsiDrive::ReadSectorsAudioOnly(DWORD startLBA, DWORD count, BYTE* audio) {
	if (count == 0 || count > 32) return false;

	BYTE cdb[12] = {};
	DWORD bufferSize = AUDIO_SECTOR_SIZE * count;

	cdb[0] = SCSI_READ_CD;
	cdb[1] = 0x04; // Expected sector type: CD-DA
	cdb[2] = (startLBA >> 24) & 0xFF;
	cdb[3] = (startLBA >> 16) & 0xFF;
	cdb[4] = (startLBA >> 8) & 0xFF;
	cdb[5] = startLBA & 0xFF;
	cdb[6] = (count >> 16) & 0xFF;
	cdb[7] = (count >> 8) & 0xFF;
	cdb[8] = count & 0xFF;
	cdb[9] = 0xF8; // User data + header + EDC/ECC
	cdb[10] = 0x00; // No subchannel

	return SendSCSI(cdb, 12, audio, bufferSize);
}

bool ScsiDrive::SeekToLBA(DWORD lba) {
	BYTE cdb[10] = {};
	cdb[0] = 0x2B; // SEEK(10)
	cdb[2] = (lba >> 24) & 0xFF;
	cdb[3] = (lba >> 16) & 0xFF;
	cdb[4] = (lba >> 8) & 0xFF;
	cdb[5] = lba & 0xFF;
	return SendSCSI(cdb, 10, nullptr, 0, false);
}

// Q subchannel read with Expected Sector Type = 0 (any).  Requests only the
// 16-byte formatted Q subchannel — no user data — so it works on both audio
// and data sectors.  Used by the TOC-less scanner to see through data tracks
// that reject CD-DA-typed reads.
bool ScsiDrive::ReadSectorQAnyType(DWORD lba, int& qTrack, int& qIndex) {
	BYTE cdb[12] = {};
	BYTE buffer[16] = {};

	cdb[0] = SCSI_READ_CD;
	cdb[1] = 0x00;                  // Expected Sector Type = any
	cdb[2] = (lba >> 24) & 0xFF;
	cdb[3] = (lba >> 16) & 0xFF;
	cdb[4] = (lba >> 8) & 0xFF;
	cdb[5] = lba & 0xFF;
	cdb[8] = 1;
	cdb[9] = 0x00;                  // No user data, no header, no EDC
	cdb[10] = 0x02;                 // Formatted Q subchannel only

	if (!SendSCSI(cdb, 12, buffer, 16)) return false;

	BYTE adr = buffer[0] & 0x0F;
	if (adr != 1) return false;

	if ((buffer[1] & 0xF0) > 0x90 || (buffer[1] & 0x0F) > 0x09) return false;
	if ((buffer[2] & 0xF0) > 0x90 || (buffer[2] & 0x0F) > 0x09) return false;

	qTrack = BcdToBin(buffer[1]);
	qIndex = BcdToBin(buffer[2]);
	return true;
}

// ── MMC structure commands ──────────────────────────────────────────────────
// These query the drive's firmware cache, not the disc surface.  They complete
// in milliseconds and work even on discs with damaged/illegal TOCs — the drive
// has already parsed the lead-in during spin-up and cached the results.

bool ScsiDrive::ReadDiscCapacity(DWORD& lastLBA, int& sessions, int& lastTrack) {
	BYTE cdb[10] = {};
	BYTE buffer[34] = {};

	cdb[0] = 0x51;
	cdb[1] = 0x00;
	cdb[7] = 0x00;
	cdb[8] = sizeof(buffer);

	if (!SendSCSI(cdb, 10, buffer, sizeof(buffer))) return false;

	lastTrack = buffer[6];
	if (lastTrack == 0) lastTrack = buffer[5];

	sessions = buffer[10] - buffer[9] + 1;
	if (sessions < 1) sessions = 1;

	lastLBA = (static_cast<DWORD>(buffer[16]) << 24) |
		(static_cast<DWORD>(buffer[17]) << 16) |
		(static_cast<DWORD>(buffer[18]) << 8) |
		static_cast<DWORD>(buffer[19]);

	if (lastLBA == 0) {
		lastLBA = (static_cast<DWORD>(buffer[20]) << 24) |
			(static_cast<DWORD>(buffer[21]) << 16) |
			(static_cast<DWORD>(buffer[22]) << 8) |
			static_cast<DWORD>(buffer[23]);
	}

	// Reject obviously invalid values — a CD cannot exceed ~90 minutes
	// (405,000 sectors).  Copy-protected TOCs often return 0xFFFFFFFF or
	// other absurd LBAs.  Also reject 0 (field not populated).
	if (lastLBA == 0 || lastLBA > 405000) {
		lastLBA = 0;
	}
	if (lastTrack > 99 || lastTrack < 1) {
		lastTrack = 0;
	}
	if (sessions > 10) {
		sessions = 1;  // No real CD has >10 sessions
	}

	return lastLBA > 0 || lastTrack > 0;
}

bool ScsiDrive::ReadTrackInfo(int trackNumber, DWORD& startLBA, DWORD& trackLength,
	bool& isAudio, int& session, int& mode) {
	// READ TRACK INFORMATION (0x52) — per-track metadata from drive firmware
	BYTE cdb[10] = {};
	BYTE buffer[36] = {};

	cdb[0] = 0x52;                   // READ TRACK INFORMATION
	cdb[1] = 0x01;                   // Address/Number Type = track number
	cdb[4] = static_cast<BYTE>(trackNumber);
	cdb[7] = 0x00;
	cdb[8] = sizeof(buffer);         // Allocation length

	if (!SendSCSI(cdb, 10, buffer, sizeof(buffer))) return false;

	// Byte 2: session number this track belongs to
	session = buffer[2];

	// Byte 5, bits 3-0: track mode
	//   0x00 or 0x02 = audio (2 ch without/with pre-emphasis)
	//   0x01 or 0x03 = audio (4 ch)
	//   0x04 = data, uninterrupted
	//   0x05 = data, incremental
	mode = buffer[5] & 0x0F;
	isAudio = (mode <= 0x03);

	// Bytes 8-11: track start address (LBA, MSB first)
	startLBA = (static_cast<DWORD>(buffer[8]) << 24) |
		(static_cast<DWORD>(buffer[9]) << 16) |
		(static_cast<DWORD>(buffer[10]) << 8) |
		static_cast<DWORD>(buffer[11]);

	// Bytes 24-27: track size in sectors (MSB first)
	trackLength = (static_cast<DWORD>(buffer[24]) << 24) |
		(static_cast<DWORD>(buffer[25]) << 16) |
		(static_cast<DWORD>(buffer[26]) << 8) |
		static_cast<DWORD>(buffer[27]);

	return true;
}