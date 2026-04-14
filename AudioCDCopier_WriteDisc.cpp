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
// WriteDisc - Write disc from .bin/.cue/.sub files
// ============================================================================
bool AudioCDCopier::WriteDisc(const std::wstring& binFile,
	const std::wstring& cueFile, const std::wstring& subFile,
	int speed, bool usePowerCalibration) {

	Console::BoxHeading("Write Disc from Files");

	// ── Check disc is empty and writable ────────────────────────────
	{
		Console::Info("Checking disc media status...\n");

		WriteDiscInternal::WaitForDriveReady(m_drive, 10);

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
		if (tracks.back().endLBA == 0) {
			// Last track in CUE — endLBA not set by parser, use BIN file size
			tracks.back().endLBA = totalSectors - 1;
		}
		else if (tracks.back().endLBA + 1 < totalSectors) {
			// Data tracks were filtered — BIN is larger than audio portion
			Console::Info("Trimming to audio content: ");
			std::cout << (tracks.back().endLBA + 1) << " of " << totalSectors << " sectors\n";
			totalSectors = tracks.back().endLBA + 1;
		}
	}

	// ── Verify disc has enough capacity for the image ───────────────
	// (after CUE parse so totalSectors reflects any trim)
	{
		// READ TRACK INFORMATION: type=1 (track number), track 0xFF (invisible/blank)
		BYTE trackInfoCmd[10] = { 0x52, 0x01, 0x00, 0x00, 0x00, 0xFF, 0x00, 0x00, 0x24, 0x00 };
		BYTE trackInfoResp[36] = { 0 };
		if (m_drive.SendSCSI(trackInfoCmd, sizeof(trackInfoCmd),
			trackInfoResp, sizeof(trackInfoResp), true)) {
			DWORD freeBlocks = (static_cast<DWORD>(trackInfoResp[24]) << 24) |
				(static_cast<DWORD>(trackInfoResp[25]) << 16) |
				(static_cast<DWORD>(trackInfoResp[26]) << 8) |
				static_cast<DWORD>(trackInfoResp[27]);

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

	// ── Check CD-Text write capability early (before write mode setup) ──
	bool canWriteCDText = false;
	if (WriteDiscInternal::HasCDTextContent(discTitle, discPerformer, tracks)) {
		DriveCapabilities caps;
		canWriteCDText = m_drive.DetectCapabilities(caps) && caps.supportsWriteCDText;
		if (!canWriteCDText) {
			Console::Warning("Drive does not advertise CD-Text write support — skipping\n");
		}
	}

	// Set drive write speed BEFORE power calibration (OPC is speed-dependent)
	m_drive.SetSpeed(speed, speed);
	Console::Success("Drive speed set to ");
	std::cout << speed << "x\n";

	// Power calibration
	if (usePowerCalibration) {
		if (!PerformPowerCalibration()) {
			return false;
		}
	}

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

	// ── Send CD-Text after write mode setup, using pre-cached capability ──
	if (canWriteCDText) {
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

	Console::Info("\nWriting sectors...\n");
	if (!WriteAudioSectors(binFile, subFile, tracks, totalSectors,
		hasSubchannel, needsDeinterleave, subchannelMode, discMCN)) {
		Console::Error("Failed to write sectors\n");
		return false;
	}

	return VerifyWriteCompletion(binFile);
}