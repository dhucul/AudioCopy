// ============================================================================
// ScsiDrive.h - Low-level SCSI drive communication
// ============================================================================
#pragma once

#include "ScsiTypes.h"
#include "DriveTypes.h"
#include "Constants.h"
#include <windows.h>
#include <ntddcdrm.h>
#include <ntddscsi.h>
#include <vector>
#include <string>

class ScsiDrive {
private:
	HANDLE m_handle = INVALID_HANDLE_VALUE;
	WORD m_currentSpeed = CD_SPEED_MAX;
	C2Mode m_c2Mode = C2Mode::NotSupported;
	bool m_c1BlockErrorsAvailable = false;     // True if bytes 294-295 contain valid C1 data
	int m_maxRetries = 5;
	int m_retryDelayMs = 100;

public:
	// ── Type aliases for backward compatibility ──────────────
	using C2ReadOptions = ::C2ReadOptions;  // Re-export from ScsiTypes.h

	ScsiDrive() = default;
	~ScsiDrive() { Close(); }

	// Non-copyable
	ScsiDrive(const ScsiDrive&) = delete;
	ScsiDrive& operator=(const ScsiDrive&) = delete;

	// ── Core operations ──────────────────────────────────────
	bool Open(wchar_t driveLetter);
	void Close();
	bool IsOpen() const { return m_handle != INVALID_HANDLE_VALUE; }

	// ── Speed control ────────────────────────────────────────
	void SetSpeed(int multiplier, int writeMultiplier = -1);
	WORD GetCurrentSpeed() const { return m_currentSpeed; }
	bool GetActualSpeed(WORD& readSpeed, WORD& writeSpeed);

	// ── Sector reading ───────────────────────────────────────
	bool ReadSector(DWORD lba, BYTE* audio, BYTE* subchannel);
	bool ReadSectorWithC2(DWORD lba, BYTE* audio, BYTE* subchannel, int& c2Errors);
	bool ReadSectorAudioOnly(DWORD lba, BYTE* audio);
	bool ReadSectorsAudioOnly(DWORD startLBA, DWORD count, BYTE* audio);
	bool ReadDataSector(DWORD lba, BYTE* data);
	bool ReadSectorQ(DWORD lba, int& qTrack, int& qIndex);
	bool ReadSectorQSingle(DWORD lba, int& qTrack, int& qIndex);
	bool ReadSectorQAdaptive(DWORD lba, int& qTrack, int& qIndex,
		DWORD pregapLBA, DWORD startLBA);
	bool ReadSectorQAnyType(DWORD lba, int& qTrack, int& qIndex);

	// ── Enhanced C2 reading ──────────────────────────────────
	bool ReadSectorWithC2Ex(DWORD lba, BYTE* audio, BYTE* subchannel, int& c2Errors,
		BYTE* c2Raw, const C2ReadOptions& options,
		BYTE* outSenseKey = nullptr, BYTE* outASC = nullptr, BYTE* outASCQ = nullptr,
		int* outC1BlockErrors = nullptr, int* outC2BlockErrors = nullptr);
	bool ReadSectorWithC2ExMultiPass(DWORD lba, BYTE* audio, BYTE* subchannel,
		int& c2Errors, BYTE* c2Raw, const C2ReadOptions& options,
		BYTE* outSenseKey = nullptr, BYTE* outASC = nullptr, BYTE* outASCQ = nullptr);
	bool PlextorReadC2(DWORD lba, BYTE* audio, int& c2Errors, BYTE* c2Raw, bool countBytes,
		BYTE* outSenseKey = nullptr, BYTE* outASC = nullptr, BYTE* outASCQ = nullptr,
		int* outC1BlockErrors = nullptr, int* outC2BlockErrors = nullptr);
	bool ValidateC2Accuracy(DWORD testLBA);
	bool IsPlextor();
	bool SupportsC1BlockErrors() const;

	// ── Plextor Q-Check hardware quality scan ────────────────
	// Uses vendor commands 0xE9 (start scan) and 0xEB (poll results) to
	// perform the same C1/C2/CU measurement that QPXTool uses.  The drive
	// scans at ~1x internally; no audio data is transferred.
	bool PlextorQCheckStart(DWORD startLBA, DWORD endLBA);
	bool PlextorQCheckPoll(int& c1, int& c2, int& cu, DWORD& currentLBA, bool& scanDone);
	bool PlextorQCheckStop();
	bool SupportsQCheck();

	// ── LiteOn/MediaTek quality scan (0xDF vendor command) ───
	// Used by QPXTool for MediaTek-based LiteOn, ASUS, and Plextor OEM
	// drives.  Similar to Q-Check but uses a different command set.
	bool SupportsLiteOnScan();
	bool LiteOnScanStart(DWORD startLBA, DWORD endLBA);
	bool LiteOnScanPoll(int& c1, int& c2, int& cu, DWORD& currentLBA, bool& scanDone);
	bool LiteOnScanStop();

	// ── Drive capabilities ───────────────────────────────────────
	bool CheckC2Support();
	bool GetDriveInfo(std::string& vendor, std::string& model);
	bool DetectCapabilities(DriveCapabilities& caps);
	bool GetModePage2A(std::vector<BYTE>& pageData);
	bool TestOverread(bool leadIn);

	// ── Chipset / controller identification ──────────────────────
	bool DetectChipset(ChipsetInfo& info);

	// ── Drive offset detection ───────────────────────────────
	bool DetectDriveOffset(OffsetDetectionResult& result);
	bool LookupAccurateRipOffset(DriveOffsetInfo& info);
	bool DetectOffsetFromPregap(int trackStartLBA, int& estimatedOffset);

	// ── Media control ────────────────────────────────────────
	bool Eject();
	bool SpinDown();
	bool GetMediaProfile(WORD& profileCode, std::string& profileName);
	bool RequestSenseProgress(BYTE& senseKey, BYTE& asc, BYTE& ascq, int& progressPercent);

	// ── MMC disc structure queries (no sector I/O) ───────────
	bool ReadDiscCapacity(DWORD& lastLBA, int& sessions, int& lastTrack);
	bool ReadTrackInfo(int trackNumber, DWORD& startLBA, DWORD& trackLength,
		bool& isAudio, int& session, int& mode);

	// ── Raw SCSI access ──────────────────────────────────────
	bool SendSCSI(void* cdb, BYTE cdbLength, void* buffer, DWORD bufferSize, bool dataIn = true);
	bool SendSCSIWithSense(void* cdb, BYTE cdbLength, void* buffer, DWORD bufferSize,
		BYTE* senseKey, BYTE* asc, BYTE* ascq, bool dataIn = true);
	bool SeekToLBA(DWORD lba);

	// ── Enhanced error handling ──────────────────────────────
	bool GetMediaStatus(DriveHealthCheck& status);
	bool TestUnitReady();
	bool WaitForDriveReady(int timeoutSeconds = 30);
	std::string GetSenseDescription(BYTE senseKey, BYTE asc, BYTE ascq);

	// ── Retry configuration ──────────────────────────────────
	void SetMaxRetries(int retries) { m_maxRetries = retries; }
	void SetRetryDelay(int delayMs) { m_retryDelayMs = delayMs; }

	// Get drive-specific configuration if available
	bool GetDriveSpecificConfig(DriveSpecificConfig& config) const;
	
	// Apply drive-specific settings for optimal audio extraction
	bool ApplyOptimalAudioSettings();

	// Display user-friendly recommendations for this drive
	void DisplayDriveRecommendations() const;
	
	// Get recommendation text for current drive
	std::string GetDriveRecommendationText() const;

private:
	bool ReadSectorQRaw(DWORD lba, int& qTrack, int& qIndex);
	bool ParseRawSubchannel(const BYTE* sub, int& qTrack, int& qIndex);
	bool ProbeC1BlockErrors();
};