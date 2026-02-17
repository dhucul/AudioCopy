#define NOMINMAX
#include "AudioCDCopier.h"
#include "ConsoleColors.h"
#include "Progress.h"
#include "InterruptHandler.h"
#include "WriteDiscInternal.h"
#include <iostream>
#include <fstream>
#include <vector>
#include <windows.h>

// ============================================================================
// Helper: Wait for drive to become ready (poll TEST UNIT READY)
// ============================================================================
static bool WaitForDriveReady(ScsiDrive& drive, int timeoutSeconds) {
	for (int i = 0; i < timeoutSeconds * 4; i++) {
		BYTE testCmd[6] = { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };
		BYTE sk = 0, asc = 0, ascq = 0;
		if (drive.SendSCSIWithSense(testCmd, sizeof(testCmd), nullptr, 0,
			&sk, &asc, &ascq, true)) {
			return true;
		}
		if (sk == 0x02 && asc == 0x04) {
			Sleep(250);
			continue;
		}
		return false;
	}
	return false;
}

// ============================================================================
// Helper: Flush drive write buffer via SYNCHRONIZE CACHE (0x35)
// ============================================================================
static bool SynchronizeCache(ScsiDrive& drive) {
	BYTE cdb[10] = { 0x35, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };
	BYTE sk = 0, asc = 0, ascq = 0;
	if (!drive.SendSCSIWithSense(cdb, sizeof(cdb), nullptr, 0, &sk, &asc, &ascq, false)) {
		if (sk == 0x02 && asc == 0x04) {
			return WaitForDriveReady(drive, 60);
		}
		return false;
	}
	return true;
}

// ============================================================================
// Helper: Deinterleave raw P-W subchannel to packed format
// ============================================================================
static void DeinterleaveSubchannel(const BYTE* raw, BYTE* packed) {
	memset(packed, 0, SUBCHANNEL_SIZE);
	for (int i = 0; i < 96; i++) {
		for (int ch = 0; ch < 8; ch++) {
			if (raw[i] & (0x80 >> ch)) {
				int outByte = ch * 12 + (i / 8);
				int outBit = 7 - (i % 8);
				packed[outByte] |= (1 << outBit);
			}
		}
	}
}

// ============================================================================
// WriteDisc - Write disc from .bin/.cue/.sub files
// ============================================================================
bool AudioCDCopier::WriteDisc(const std::wstring& binFile,
	const std::wstring& cueFile, const std::wstring& subFile,
	int speed, bool usePowerCalibration) {

	Console::BoxHeading("Write Disc from Files");

	// Verify input files exist
	std::ifstream binStream(binFile, std::ios::binary);
	if (!binStream.is_open()) {
		Console::Error("Cannot open .bin file: ");
		std::wcout << binFile << L"\n";
		return false;
	}
	binStream.seekg(0, std::ios::end);
	long long fileSize = binStream.tellg();
	binStream.close();

	DWORD totalSectors = static_cast<DWORD>(fileSize / AUDIO_SECTOR_SIZE);

	// Determine if we can use Raw DAO mode with subchannel data
	bool hasSubchannel = false;
	bool needsDeinterleave = false;
	if (!subFile.empty()) {
		std::ifstream subStream(subFile, std::ios::binary);
		if (subStream.is_open()) {
			subStream.seekg(0, std::ios::end);
			long long subSize = subStream.tellg();
			subStream.close();

			long long expectedSubSize = static_cast<long long>(totalSectors) * SUBCHANNEL_SIZE;
			if (subSize >= expectedSubSize) {
				hasSubchannel = true;
				Console::Success("Subchannel file validated (");
				std::cout << (subSize / 1024) << " KB, " << totalSectors << " sectors)\n";
			}
			else {
				Console::Warning("Subchannel file size mismatch (expected ");
				std::cout << expectedSubSize << " bytes, got " << subSize
					<< ") — writing without subchannel\n";
			}
		}
		else {
			Console::Warning("Cannot open .sub file — writing without subchannel data\n");
		}
	}

	// Parse CUE sheet — also extracts TITLE/PERFORMER for CD-Text
	std::vector<TrackWriteInfo> tracks;
	std::string discTitle, discPerformer;
	if (!ParseCueSheet(cueFile, tracks, discTitle, discPerformer)) {
		Console::Error("Failed to parse CUE sheet\n");
		return false;
	}

	// Set last track endLBA now that we know totalSectors
	if (!tracks.empty()) {
		tracks.back().endLBA = totalSectors - 1;
	}

	// Power calibration
	if (usePowerCalibration) {
		if (!PerformPowerCalibration()) {
			return false;
		}
	}

	// Set drive write speed
	m_drive.SetSpeed(speed);
	Console::Success("Drive speed set to ");
	std::cout << speed << "x\n";

	int subchannelMode = 0;

	if (hasSubchannel) {
		struct WriteMode {
			int mode;
			bool deinterleave;
			const char* description;
		};

		const WriteMode candidates[] = {
			{ 1, true,  "Raw DAO + packed P-W subchannel" },
			{ 2, false, "Raw DAO + raw P-W subchannel"    },
			{ 3, true,  "SAO + packed P-W subchannel"     },
			{ 4, false, "SAO + raw P-W subchannel"        },
		};

		bool modeFound = false;
		for (const auto& wm : candidates) {
			Console::Info("Trying ");
			std::cout << wm.description << "...\n";

			if (!WriteDiscInternal::PrepareDriveForWrite(m_drive, wm.mode)) {
				Console::Info("  MODE SELECT rejected — skipping\n");
				continue;
			}

			if (!WriteDiscInternal::BuildAndSendCueSheet(m_drive, tracks, totalSectors, wm.mode)) {
				Console::Info("  CUE sheet rejected — skipping\n");
				continue;
			}

			subchannelMode = wm.mode;
			needsDeinterleave = wm.deinterleave;
			Console::Success("Drive accepts ");
			std::cout << wm.description << "\n";
			modeFound = true;
			break;
		}

		if (!modeFound) {
			Console::Warning("Drive does not support subchannel writing\n");
			Console::Info("Subchannel data from .sub file will NOT be written\n");
			hasSubchannel = false;
		}
	}

	if (!hasSubchannel) {
		if (!WriteDiscInternal::PrepareDriveForWrite(m_drive, 0)) {
			return false;
		}

		Console::Info("\nSending disc layout to drive...\n");
		if (!WriteDiscInternal::BuildAndSendCueSheet(m_drive, tracks, totalSectors, 0)) {
			Console::Error("Drive rejected disc layout\n");
			return false;
		}
	}

	if (WriteDiscInternal::HasCDTextContent(discTitle, discPerformer, tracks)) {
		Console::Info("Building CD-Text from CUE metadata...\n");
		std::vector<BYTE> cdTextPacks = WriteDiscInternal::BuildCDTextPacks(discTitle, discPerformer, tracks);

		if (!cdTextPacks.empty()) {
			Console::Info("CD-Text: ");
			std::cout << (cdTextPacks.size() / 18) << " packs (";
			if (!discPerformer.empty()) std::cout << discPerformer;
			if (!discPerformer.empty() && !discTitle.empty()) std::cout << " - ";
			if (!discTitle.empty()) std::cout << discTitle;
			std::cout << ")\n";

			if (!WriteDiscInternal::SendCDTextToDevice(m_drive, cdTextPacks)) {
				Console::Warning("CD-Text will not be written (drive rejected data)\n");
				Console::Info("Audio data will still be written normally\n");
			}
		}
	}

	Console::Info("\nWriting audio sectors...\n");
	if (!WriteAudioSectors(binFile, subFile, tracks, totalSectors,
		hasSubchannel, needsDeinterleave)) {
		Console::Error("Failed to write audio sectors\n");
		return false;
	}

	return VerifyWriteCompletion(binFile);
}

// ============================================================================
// WriteAudioSectors - Write pregap silence + audio data (+ optional subchannel)
// ============================================================================
bool AudioCDCopier::WriteAudioSectors(const std::wstring& binFile,
	const std::wstring& subFile,
	const std::vector<TrackWriteInfo>& tracks,
	DWORD totalSectors,
	bool hasSubchannel,
	bool needsDeinterleave) {

	std::ifstream binInput(binFile, std::ios::binary);
	if (!binInput.is_open()) {
		Console::Error("Cannot open binary file\n");
		return false;
	}

	std::ifstream subInput;
	if (hasSubchannel) {
		subInput.open(subFile, std::ios::binary);
		if (!subInput.is_open()) {
			Console::Error("Cannot open subchannel file\n");
			binInput.close();
			return false;
		}
	}

	const DWORD sectorSize = hasSubchannel ? RAW_SECTOR_SIZE : AUDIO_SECTOR_SIZE;

	constexpr DWORD PREGAP_SECTORS = 150;
	DWORD writeTotalSectors = PREGAP_SECTORS + totalSectors;

	Console::Info("Binary file size: ");
	long long fileSize = static_cast<long long>(totalSectors) * AUDIO_SECTOR_SIZE;
	std::cout << (fileSize / (1024 * 1024)) << " MB (" << totalSectors << " sectors)\n";
	Console::Info("Total write: ");
	std::cout << writeTotalSectors << " sectors (150 pregap + " << totalSectors << " audio)\n";

	if (hasSubchannel) {
		Console::Info("Write mode: 2448 bytes/sector (2352 audio + 96 subchannel");
		if (needsDeinterleave) std::cout << ", deinterleaving";
		std::cout << ")\n";
	}
	else {
		Console::Info("Write mode: SAO (2352 bytes/sector, drive-generated subchannel)\n");
	}

	if (!WaitForDriveReady(m_drive, 10)) {
		Console::Warning("Drive not ready after CUE sheet (attempting write anyway)\n");
	}

	ProgressIndicator progress(35);
	progress.SetLabel("Writing");
	progress.Start();

	const DWORD SECTORS_PER_WRITE = hasSubchannel ? 20 : 27;
	std::vector<BYTE> writeBuffer(sectorSize * SECTORS_PER_WRITE);
	BYTE rawSub[SUBCHANNEL_SIZE];

	DWORD sectorsWritten = 0;
	int32_t currentLBA = -150;
	int consecutiveErrors = 0;
	size_t currentTrackIdx = 0;

	while (sectorsWritten < writeTotalSectors) {
		if (InterruptHandler::Instance().IsInterrupted()) {
			Console::Error("\nWrite operation cancelled by user\n");
			progress.Finish(false);
			binInput.close();
			if (hasSubchannel) subInput.close();
			return false;
		}

		DWORD remaining = writeTotalSectors - sectorsWritten;
		DWORD batchSize = (remaining < SECTORS_PER_WRITE) ? remaining : SECTORS_PER_WRITE;

		for (DWORD s = 0; s < batchSize; s++) {
			BYTE* dest = writeBuffer.data() + s * sectorSize;
			DWORD globalSector = sectorsWritten + s;

			if (globalSector < PREGAP_SECTORS) {
				memset(dest, 0x00, sectorSize);
			}
			else {
				binInput.read(reinterpret_cast<char*>(dest), AUDIO_SECTOR_SIZE);
				size_t audioRead = binInput.gcount();
				if (audioRead < AUDIO_SECTOR_SIZE) {
					std::fill(dest + audioRead, dest + AUDIO_SECTOR_SIZE, 0x00);
				}

				if (hasSubchannel) {
					BYTE* subDest = dest + AUDIO_SECTOR_SIZE;

					if (needsDeinterleave) {
						subInput.read(reinterpret_cast<char*>(rawSub), SUBCHANNEL_SIZE);
						size_t subRead = subInput.gcount();
						if (subRead < SUBCHANNEL_SIZE) {
							std::fill(rawSub + subRead, rawSub + SUBCHANNEL_SIZE, 0x00);
						}
						DeinterleaveSubchannel(rawSub, subDest);
					}
					else {
						subInput.read(reinterpret_cast<char*>(subDest), SUBCHANNEL_SIZE);
						size_t subRead = subInput.gcount();
						if (subRead < SUBCHANNEL_SIZE) {
							std::fill(subDest + subRead, subDest + SUBCHANNEL_SIZE, 0x00);
						}
					}
				}
			}
		}

		DWORD transferBytes = batchSize * sectorSize;

		BYTE writeCmd[10] = { 0x2A, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };
		DWORD lbaUnsigned = static_cast<DWORD>(currentLBA);
		writeCmd[2] = static_cast<BYTE>((lbaUnsigned >> 24) & 0xFF);
		writeCmd[3] = static_cast<BYTE>((lbaUnsigned >> 16) & 0xFF);
		writeCmd[4] = static_cast<BYTE>((lbaUnsigned >> 8) & 0xFF);
		writeCmd[5] = static_cast<BYTE>(lbaUnsigned & 0xFF);
		writeCmd[7] = static_cast<BYTE>((batchSize >> 8) & 0xFF);
		writeCmd[8] = static_cast<BYTE>(batchSize & 0xFF);

		BYTE senseKey = 0, asc = 0, ascq = 0;
		bool writeOk = m_drive.SendSCSIWithSense(writeCmd, sizeof(writeCmd),
			writeBuffer.data(), transferBytes, &senseKey, &asc, &ascq, false);

		if (!writeOk) {
			if (senseKey == 0x02 && asc == 0x04) {
				if (!WaitForDriveReady(m_drive, 30)) {
					consecutiveErrors++;
					if (consecutiveErrors >= 5) {
						Console::Error("\nDrive not recovering - aborting\n");
						progress.Finish(false);
						binInput.close();
						if (hasSubchannel) subInput.close();
						return false;
					}
					continue;
				}
				DWORD binSector = (sectorsWritten >= PREGAP_SECTORS) ? sectorsWritten - PREGAP_SECTORS : 0;
				binInput.seekg(static_cast<long long>(binSector) * AUDIO_SECTOR_SIZE);
				if (hasSubchannel)
					subInput.seekg(static_cast<long long>(binSector) * SUBCHANNEL_SIZE);
				consecutiveErrors = 0;
				continue;
			}

			consecutiveErrors++;
			Console::Error("\nWrite failed at LBA ");
			std::cout << currentLBA << " (";
			std::cout << m_drive.GetSenseDescription(senseKey, asc, ascq) << ")\n";

			if (consecutiveErrors >= 5) {
				Console::Error("Too many consecutive write errors - aborting\n");
				progress.Finish(false);
				binInput.close();
				if (hasSubchannel) subInput.close();
				return false;
			}

			DWORD binSector = (sectorsWritten >= PREGAP_SECTORS) ? sectorsWritten - PREGAP_SECTORS : 0;
			binInput.seekg(static_cast<long long>(binSector) * AUDIO_SECTOR_SIZE);
			if (hasSubchannel)
				subInput.seekg(static_cast<long long>(binSector) * SUBCHANNEL_SIZE);
			Sleep(1000);
			continue;
		}

		consecutiveErrors = 0;
		sectorsWritten += batchSize;
		currentLBA += batchSize;
		progress.Update(sectorsWritten, writeTotalSectors);

		DWORD binPosition = (sectorsWritten > PREGAP_SECTORS) ? sectorsWritten - PREGAP_SECTORS : 0;
		while (currentTrackIdx + 1 < tracks.size() &&
			binPosition >= tracks[currentTrackIdx + 1].startLBA) {
			currentTrackIdx++;
			Console::Info("\n  Track ");
			std::cout << tracks[currentTrackIdx].trackNumber << " reached\n";
		}
	}

	progress.Finish(true);
	binInput.close();
	if (hasSubchannel) subInput.close();

	Console::Success("Successfully wrote ");
	std::cout << sectorsWritten << " sectors";
	if (hasSubchannel) std::cout << " (with subchannel data)";
	std::cout << "\n";
	return true;
}

// ============================================================================
// VerifyWriteCompletion - Flush cache and close session
// ============================================================================
bool AudioCDCopier::VerifyWriteCompletion(const std::wstring& /*binFile*/) {
	Console::Info("Flushing write cache...\n");
	if (!SynchronizeCache(m_drive)) {
		Console::Warning("Cache flush reported failure (disc may still finalize)\n");
	}

	WaitForDriveReady(m_drive, 60);

	Console::Info("Closing session (writing lead-out)...\n");
	BYTE closeCmd[10] = { 0x5B, 0x00, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };
	BYTE senseKey = 0, asc = 0, ascq = 0;

	if (!m_drive.SendSCSIWithSense(closeCmd, sizeof(closeCmd), nullptr, 0,
		&senseKey, &asc, &ascq, false)) {
		if (senseKey == 0x02 && asc == 0x04) {
			Console::Info("Waiting for finalization");
		}
		else {
			Console::Warning("CLOSE SESSION command returned: ");
			std::cout << m_drive.GetSenseDescription(senseKey, asc, ascq) << "\n";
		}
	}

	Console::Info("Finalizing disc");
	for (int i = 0; i < 120; i++) {
		BYTE testCmd[6] = { 0x00 };
		if (m_drive.SendSCSI(testCmd, sizeof(testCmd), nullptr, 0, true)) {
			std::cout << "\n";
			Console::Success("Disc finalized successfully\n");
			return true;
		}
		if (i % 5 == 0) std::cout << ".";
		Sleep(1000);
	}

	std::cout << "\n";
	Console::Warning("Finalization timeout — disc may still be usable\n");
	return true;
}

// ============================================================================
// VerifyWrittenDisc - Read back and verify written sectors
// ============================================================================
bool AudioCDCopier::VerifyWrittenDisc(const std::vector<TrackWriteInfo>& tracks) {
	if (tracks.empty()) {
		Console::Warning("No tracks to verify\n");
		return false;
	}

	Console::Info("Verifying written sectors...\n");

	ProgressIndicator progress(35);
	progress.SetLabel("Verifying");
	progress.Start();

	int verifyCount = 0;
	int successCount = 0;
	DWORD totalChecks = static_cast<DWORD>(tracks.size() * 2);

	for (const auto& track : tracks) {
		BYTE audio[AUDIO_SECTOR_SIZE] = { 0 };
		BYTE subchannel[SUBCHANNEL_SIZE] = { 0 };

		if (m_drive.ReadSector(track.startLBA, audio, subchannel)) {
			successCount++;
		}
		verifyCount++;
		progress.Update(verifyCount, totalChecks);

		if (track.endLBA > track.startLBA) {
			if (m_drive.ReadSector(track.endLBA, audio, subchannel)) {
				successCount++;
			}
		}
		else {
			successCount++;
		}
		verifyCount++;
		progress.Update(verifyCount, totalChecks);
	}

	progress.Finish(successCount == verifyCount);
	Console::Success("Verification: ");
	std::cout << successCount << "/" << verifyCount << " sectors readable\n";
	return successCount == verifyCount;
}