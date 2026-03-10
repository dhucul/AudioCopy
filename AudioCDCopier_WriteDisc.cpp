#define NOMINMAX
#include "AudioCDCopier.h"
#include "ConsoleColors.h"
#include "Progress.h"
#include "InterruptHandler.h"
#include "WriteDiscInternal.h"
#include <conio.h>
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
		// NOT READY (0x02) with "becoming ready" (ASC 0x04) — keep polling
		if (sk == 0x02 && asc == 0x04) {
			Sleep(250);
			continue;
		}
		// UNIT ATTENTION (0x06) — transient after blank/OPC/media change, retry
		if (sk == 0x06) {
			Sleep(250);
			continue;
		}
		// NOT READY with "medium not present" (0x3A) — may be transient during
		// post-blank media re-detection on some drives
		if (sk == 0x02 && asc == 0x3A) {
			Sleep(500);
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
		// Drive not ready or unit attention — wait and let finalization continue
		if ((sk == 0x02 && asc == 0x04) || sk == 0x06) {
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
// Helper: Find which track owns a given bin-file sector position
// ============================================================================
static size_t FindTrackForSector(const std::vector<AudioCDCopier::TrackWriteInfo>& tracks,
	DWORD binSector, bool& isInPregap) {
	isInPregap = false;
	for (size_t i = tracks.size(); i > 0; i--) {
		size_t idx = i - 1;
		if (binSector >= tracks[idx].startLBA) return idx;
		if (tracks[idx].hasPregap && binSector >= tracks[idx].pregapLBA) {
			isInPregap = true;
			return idx;
		}
	}
	return 0;
}

// ============================================================================
// WriteDisc - Write disc from .bin/.cue/.sub files
// ============================================================================
bool AudioCDCopier::WriteDisc(const std::wstring& binFile,
	const std::wstring& cueFile, const std::wstring& subFile,
	int speed, bool usePowerCalibration) {

	Console::BoxHeading("Write Disc from Files");

	// ── Check disc is empty and writable ────────────────────────────
	{
		Console::Info("Checking disc media status...\n");

		WaitForDriveReady(m_drive, 10);

		BYTE discInfoCmd[10] = { 0x51, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xFC, 0x00 };
		BYTE discInfoResp[252] = { 0 };
		BYTE sk = 0, asc = 0, ascq = 0;

		if (m_drive.SendSCSIWithSense(discInfoCmd, sizeof(discInfoCmd),
			discInfoResp, sizeof(discInfoResp), &sk, &asc, &ascq, true)) {

			BYTE discStatus = discInfoResp[2] & 0x03;
			bool isErasable = (discInfoResp[2] & 0x10) != 0;

			switch (discStatus) {
			case 0x00:
				Console::Success("Disc is empty and ready for writing\n");
				break;

			case 0x01: // Appendable -- has an open/incomplete session
				if (isErasable) {
					Console::Warning("CD-RW disc has an incomplete session\n");
					Console::Info("The disc must be blanked before DAO writing\n");
					if (!BlankRewritableDisk(speed, true)) {
						Console::Error("Failed to blank disc -- write cancelled\n");
						return false;
					}
				}
				else {
					Console::Error("CD-R already has data and cannot be erased\n");
					Console::Info("Insert a blank CD-R disc and try again\n");
					return false;
				}
				break;

			case 0x02: // Complete -- fully written
				if (isErasable) {
					Console::Warning("CD-RW disc is not empty (fully written)\n");
					Console::Info("The disc must be blanked before writing. Blank now? (y/n): ");
					char c = static_cast<char>(_getch());
					std::cout << c << "\n";
					if (tolower(c) != 'y') {
						Console::Info("Write operation cancelled\n");
						return false;
					}
					if (!BlankRewritableDisk(speed, true)) {
						Console::Error("Failed to blank disc\n");
						return false;
					}
				}
				else {
					Console::Error("CD-R is fully written and cannot be erased\n");
					Console::Info("Insert a blank CD-R disc and try again\n");
					return false;
				}
				break;

			default:
				Console::Warning("Unknown disc status (0x");
				std::cout << std::hex << static_cast<int>(discStatus)
					<< std::dec << ") -- attempting to write\n";
				break;
			}
		}
		else {
			// READ DISC INFORMATION failed -- try GET CONFIGURATION profile fallback
			BYTE profileCmd[10] = { 0x46, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x08, 0x00 };
			BYTE profileResp[8] = { 0 };

			if (m_drive.SendSCSI(profileCmd, sizeof(profileCmd),
				profileResp, sizeof(profileResp), true)) {
				WORD profile = (static_cast<WORD>(profileResp[6]) << 8) | profileResp[7];

				switch (profile) {
				case 0x08:
					Console::Error("Drive reports CD-ROM media (read-only)\n");
					return false;
				case 0x09:
					Console::Warning("CD-R detected but could not verify empty status\n");
					Console::Info("Proceeding -- write will fail if disc is not blank\n");
					break;
				case 0x0A:
					Console::Warning("CD-RW detected but could not verify empty status\n");
					Console::Info("Proceeding -- write will fail if disc is not blank\n");
					break;
				default:
					Console::Warning("Unknown media profile (0x");
					std::cout << std::hex << profile << std::dec
						<< ") -- attempting to write\n";
					break;
				}
			}
			else {
				Console::Warning("Could not determine disc status (");
				std::cout << m_drive.GetSenseDescription(sk, asc, ascq)
					<< ") -- attempting to write\n";
			}
		}
	}

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

	// ── Verify disc has enough capacity for the image ───────────────
	{
		// READ TRACK INFORMATION on the invisible track (0xFF) to get
		// writable capacity on blank media.  READ CAPACITY (0x25) only
		// works on already-written discs.
		BYTE trackInfoCmd[10] = { 0x52, 0x00, 0x01, 0x00, 0x00, 0x00, 0xFF, 0x00, 0x24, 0x00 };
		BYTE trackInfoResp[36] = { 0 };
		if (m_drive.SendSCSI(trackInfoCmd, sizeof(trackInfoCmd),
			trackInfoResp, sizeof(trackInfoResp), true)) {
			// Bytes 24-27: Free Blocks (number of writable sectors remaining)
			DWORD freeBlocks = (static_cast<DWORD>(trackInfoResp[24]) << 24) |
				(static_cast<DWORD>(trackInfoResp[25]) << 16) |
				(static_cast<DWORD>(trackInfoResp[26]) << 8) |
				static_cast<DWORD>(trackInfoResp[27]);

			// Total sectors needed: 150 pregap + audio data + ~6750 lead-out
			constexpr DWORD LEADOUT_OVERHEAD = 6750;
			constexpr DWORD PREGAP_OVERHEAD = 150;
			DWORD sectorsNeeded = totalSectors + PREGAP_OVERHEAD + LEADOUT_OVERHEAD;

			if (freeBlocks > 0 && sectorsNeeded > freeBlocks) {
				Console::Error("Image too large for disc (need ");
				std::cout << sectorsNeeded << " sectors, disc has "
					<< freeBlocks << " free)\n";
				Console::Info("Use a higher-capacity disc (e.g., 80-min or 90-min CD-R)\n");
				return false;
			}
		}
	}

	// Determine if we can use Raw mode with subchannel data
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
					<< ") -- writing without subchannel\n";
			}
		}
		else {
			Console::Warning("Cannot open .sub file -- writing without subchannel data\n");
		}
	}

	// Parse CUE sheet -- also extracts TITLE/PERFORMER/CATALOG for CD-Text and MCN
	std::vector<TrackWriteInfo> tracks;
	std::string discTitle, discPerformer, discMCN;
	if (!ParseCueSheet(cueFile, tracks, discTitle, discPerformer, discMCN)) {
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
	m_drive.SetSpeed(speed, speed);
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
			{ 1, true,  "Raw + packed P-W subchannel" },
			{ 2, false, "Raw + raw P-W subchannel"    },
			{ 3, true,  "DAO + packed P-W subchannel" },
			{ 4, false, "DAO + raw P-W subchannel"    },
		};

		bool modeFound = false;
		for (const auto& wm : candidates) {
			Console::Info("Trying ");
			std::cout << wm.description << "...\n";

			if (!WriteDiscInternal::PrepareDriveForWrite(m_drive, wm.mode)) {
				Console::Info("  MODE SELECT rejected -- skipping\n");
				continue;
			}

			if (!WriteDiscInternal::BuildAndSendCueSheet(m_drive, tracks, totalSectors, wm.mode, false)) {
				Console::Info("  CUE sheet rejected -- skipping\n");
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
		// Try Raw without subchannel first, fall back to DAO
		bool rawOk = false;
		if (WriteDiscInternal::PrepareDriveForWrite(m_drive, 5, true)) {
			if (WriteDiscInternal::BuildAndSendCueSheet(m_drive, tracks, totalSectors, 5, false)) {
				subchannelMode = 5;
				rawOk = true;
				Console::Success("Using Raw mode (no subchannel)\n");
			}
		}

		if (!rawOk) {
			if (!WriteDiscInternal::PrepareDriveForWrite(m_drive, 0)) {
				return false;
			}

			Console::Info("\nSending disc layout to drive...\n");
			if (!WriteDiscInternal::BuildAndSendCueSheet(m_drive, tracks, totalSectors, 0)) {
				Console::Error("Drive rejected disc layout\n");
				return false;
			}
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
		hasSubchannel, needsDeinterleave, subchannelMode, discMCN)) {
		Console::Error("Failed to write audio sectors\n");
		return false;
	}

	return VerifyWriteCompletion(binFile);
}

// ============================================================================
// WriteAudioSectors - Write pregap silence + audio data (+ optional subchannel)
//
// When a .sub file is provided, subchannel data is read from it.  Otherwise,
// if the write mode includes subchannel, Q-channel data is synthesized from
// the CUE sheet track list, injecting MCN and ISRC at the Red Book intervals
// (every 100 sectors at positions 99 and 49 respectively).
// ============================================================================
bool AudioCDCopier::WriteAudioSectors(const std::wstring& binFile,
	const std::wstring& subFile,
	const std::vector<TrackWriteInfo>& tracks,
	DWORD totalSectors,
	bool hasSubchannel,
	bool needsDeinterleave,
	int subchannelMode,
	const std::string& discMCN) {

	std::ifstream binInput(binFile, std::ios::binary);
	if (!binInput.is_open()) {
		Console::Error("Cannot open binary file\n");
		return false;
	}

	// Open .sub file if provided; otherwise synthesize Q-channel from CUE data
	bool hasSubFile = false;
	std::ifstream subInput;
	if (hasSubchannel && !subFile.empty()) {
		subInput.open(subFile, std::ios::binary);
		if (subInput.is_open()) {
			hasSubFile = true;
		} else {
			Console::Warning("Cannot open .sub file -- synthesizing Q-channel from CUE metadata\n");
		}
	}

	// Drive expects raw interleaved P-W (modes 2 and 4) vs packed (modes 1 and 3)
	bool driveWantsRaw = (subchannelMode == 2 || subchannelMode == 4);

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
		if (hasSubFile && needsDeinterleave) std::cout << ", deinterleaving";
		if (!hasSubFile) std::cout << ", synthesized Q-channel";
		if (!discMCN.empty()) std::cout << ", MCN: " << discMCN;
		std::cout << ")\n";
	}
	else {
		const char* modeName = (subchannelMode == 5) ? "Raw" : "DAO";
		Console::Info("Write mode: ");
		std::cout << modeName << " (2352 bytes/sector, drive-generated subchannel)\n";
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
			if (hasSubFile) subInput.close();
			return false;
		}

		DWORD remaining = writeTotalSectors - sectorsWritten;
		DWORD batchSize = (remaining < SECTORS_PER_WRITE) ? remaining : SECTORS_PER_WRITE;

		for (DWORD s = 0; s < batchSize; s++) {
			BYTE* dest = writeBuffer.data() + s * sectorSize;
			DWORD globalSector = sectorsWritten + s;

			if (globalSector < PREGAP_SECTORS) {
				// ── Pregap sector (LBA -150 to -1) ──────────────────────
				memset(dest, 0x00, sectorSize);

				if (hasSubchannel) {
					BYTE* subDest = dest + AUDIO_SECTOR_SIZE;

					int relFrame = static_cast<int>(PREGAP_SECTORS) - 1 - static_cast<int>(globalSector);
					int absFrame = static_cast<int>(globalSector);

					BYTE q12[12];
					BuildPositionQ(q12, 0x00, 1, 0,
						static_cast<BYTE>(relFrame / (75 * 60)),
						static_cast<BYTE>((relFrame / 75) % 60),
						static_cast<BYTE>(relFrame % 75),
						static_cast<BYTE>(absFrame / (75 * 60)),
						static_cast<BYTE>((absFrame / 75) % 60),
						static_cast<BYTE>(absFrame % 75));

					BuildPackedSubchannel(subDest, q12, true);  // P=1 for pregap

					if (driveWantsRaw) {
						BYTE raw96[SUBCHANNEL_SIZE];
						InterleaveSubchannel(subDest, raw96);
						memcpy(subDest, raw96, SUBCHANNEL_SIZE);
					}
				}
			}
			else {
				// ── Data sector (LBA 0+) ────────────────────────────────
				binInput.read(reinterpret_cast<char*>(dest), AUDIO_SECTOR_SIZE);
				size_t audioRead = binInput.gcount();
				if (audioRead < AUDIO_SECTOR_SIZE) {
					std::fill(dest + audioRead, dest + AUDIO_SECTOR_SIZE, 0x00);
				}

				if (hasSubchannel) {
					BYTE* subDest = dest + AUDIO_SECTOR_SIZE;

					if (hasSubFile) {
						// ── Read subchannel from .sub file ──────────────
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
					else {
						// ── Synthesize Q-channel from CUE metadata ──────
						DWORD binSector = globalSector - PREGAP_SECTORS;

						bool isInPregap = false;
						size_t trkIdx = FindTrackForSector(tracks, binSector, isInPregap);

						BYTE trackNo = static_cast<BYTE>(tracks[trkIdx].trackNumber);
						BYTE indexNo = isInPregap ? 0 : 1;

						// Relative MSF (counts down in pregap, up in index 1)
						int relFrames;
						if (isInPregap) {
							relFrames = static_cast<int>(tracks[trkIdx].startLBA) - 1
								- static_cast<int>(binSector);
						}
						else {
							relFrames = static_cast<int>(binSector)
								- static_cast<int>(tracks[trkIdx].startLBA);
						}

						// Absolute MSF (LBA 0 = 00:02:00)
						int absFrames = static_cast<int>(binSector) + 150;

						BYTE absM = static_cast<BYTE>(absFrames / (75 * 60));
						BYTE absS = static_cast<BYTE>((absFrames / 75) % 60);
						BYTE absF = static_cast<BYTE>(absFrames % 75);
						BYTE relM = static_cast<BYTE>(relFrames / (75 * 60));
						BYTE relS = static_cast<BYTE>((relFrames / 75) % 60);
						BYTE relF = static_cast<BYTE>(relFrames % 75);

						BYTE q12[12];

						// Avoid replacing position Q at track/index transitions
						// so the player always sees the boundary change
						bool atTrackBoundary = (binSector == tracks[trkIdx].startLBA)
							|| (tracks[trkIdx].hasPregap && binSector == tracks[trkIdx].pregapLBA);

						// MCN (Mode-2) at sector % 100 == 99
						// ISRC (Mode-3) at sector % 100 == 49
						if (!atTrackBoundary && !discMCN.empty() && discMCN.size() >= 13
							&& (binSector % 100 == 99)) {
							EncodeMCN(q12, discMCN.c_str(), absF);
						}
						else if (!atTrackBoundary && !tracks[trkIdx].isrcCode.empty()
							&& (binSector % 100 == 49)) {
							EncodeISRC(q12, tracks[trkIdx].isrcCode.c_str(), absF);
						}
						else {
							BuildPositionQ(q12, 0x00, trackNo, indexNo,
								relM, relS, relF, absM, absS, absF);
						}

						BuildPackedSubchannel(subDest, q12, isInPregap);

						if (driveWantsRaw) {
							BYTE raw96[SUBCHANNEL_SIZE];
							InterleaveSubchannel(subDest, raw96);
							memcpy(subDest, raw96, SUBCHANNEL_SIZE);
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
						if (hasSubFile) subInput.close();
						return false;
					}
					continue;
				}
				DWORD binSector = (sectorsWritten >= PREGAP_SECTORS) ? sectorsWritten - PREGAP_SECTORS : 0;
				binInput.seekg(static_cast<long long>(binSector) * AUDIO_SECTOR_SIZE);
				if (hasSubFile)
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
				if (hasSubFile) subInput.close();
				return false;
			}

			DWORD binSector = (sectorsWritten >= PREGAP_SECTORS) ? sectorsWritten - PREGAP_SECTORS : 0;
			binInput.seekg(static_cast<long long>(binSector) * AUDIO_SECTOR_SIZE);
			if (hasSubFile)
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
	if (hasSubFile) subInput.close();

	Console::Success("Successfully wrote ");
	std::cout << sectorsWritten << " sectors";
	if (hasSubchannel) {
		std::cout << " (with subchannel";
		if (!hasSubFile) std::cout << ", synthesized";
		std::cout << ")";
	}
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
	Console::Warning("Finalization timeout -- disc may still be usable\n");
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
	DWORD totalChecks = 0;
	for (const auto& track : tracks) {
		totalChecks++; // start sector
		if (track.endLBA > track.startLBA)
			totalChecks++; // end sector
	}

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
			verifyCount++;
		}
	}

	progress.Finish(successCount == verifyCount);
	Console::Success("Verification: ");
	std::cout << successCount << "/" << verifyCount << " sectors readable\n";
	return successCount == verifyCount;
}