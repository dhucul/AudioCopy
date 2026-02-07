// ============================================================================
// ScsiDrive.h - Low-level SCSI drive communication
// ============================================================================
#pragma once

#include "DiscTypes.h"
#include <windows.h>
#include <ntddcdrm.h>
#include <ntddscsi.h>
#include <vector>
#include <string>

// Drive offset detection structures
struct DriveOffsetInfo {
	int readOffset = 0;
	int writeOffset = 0;
	bool fromDatabase = false;
	bool supportsAccurateStream = false;
	std::string source;
};

enum class OffsetDetectionMethod {
	Unknown,
	Database,              // From AccurateRip drive database
	AccurateRipCalibration, // Auto-detected via disc verification
	PregapAnalysis,        // Estimated from pregap silence
	Manual                 // User-provided
};

struct OffsetDetectionResult {
	int offset = 0;
	int confidence = 0;  // 0-100
	OffsetDetectionMethod method = OffsetDetectionMethod::Unknown;
	std::string details;
};

enum class C2Mode {
	NotSupported,
	ErrorBlock,      // Standard C2 error block
	ErrorPointers,   // C2 error pointers (more accurate)
	PlextorD8       // Plextor vendor command
};

class ScsiDrive {
private:
	HANDLE m_handle = INVALID_HANDLE_VALUE;
	WORD m_currentSpeed = CD_SPEED_MAX;
	C2Mode m_c2Mode = C2Mode::NotSupported;  // Add this

public:
	ScsiDrive() = default;
	~ScsiDrive() { Close(); }

	// Non-copyable
	ScsiDrive(const ScsiDrive&) = delete;
	ScsiDrive& operator=(const ScsiDrive&) = delete;

	bool Open(wchar_t driveLetter);
	void Close();
	bool IsOpen() const { return m_handle != INVALID_HANDLE_VALUE; }

	// Speed control
	void SetSpeed(int multiplier);
	WORD GetCurrentSpeed() const { return m_currentSpeed; }

	// Sector reading
	bool ReadSector(DWORD lba, BYTE* audio, BYTE* subchannel);
	bool ReadSectorWithC2(DWORD lba, BYTE* audio, BYTE* subchannel, int& c2Errors);
	bool ReadSectorAudioOnly(DWORD lba, BYTE* audio);
	bool ReadDataSector(DWORD lba, BYTE* data);
	bool ReadSectorQ(DWORD lba, int& qTrack, int& qIndex);

	// Enhanced C2 reading options
	struct C2ReadOptions {
		bool multiPass = false;
		int passCount = 3;
		bool defeatCache = false;
		bool countBytes = false;  // true = PlexTools-style byte counting
	};

	bool ReadSectorWithC2Ex(DWORD lba, BYTE* audio, BYTE* subchannel, int& c2Errors,
		BYTE* c2Raw, const C2ReadOptions& options);
	bool ValidateC2Accuracy(DWORD testLBA);  // Test if drive reports C2 reliably

	// Added these method declarations
	bool ReadSectorWithC2ExMultiPass(DWORD lba, BYTE* audio, BYTE* subchannel,
		int& c2Errors, BYTE* c2Raw, const C2ReadOptions& options);
	bool PlextorReadC2(DWORD lba, BYTE* audio, int& c2Errors, BYTE* c2Raw, bool countBytes);

	// Drive capabilities
	bool CheckC2Support();
	bool GetDriveInfo(std::string& vendor, std::string& model);
	bool DetectCapabilities(DriveCapabilities& caps);
	bool GetModePage2A(std::vector<BYTE>& pageData);
	bool TestOverread(bool leadIn);

	// Drive offset detection
	bool DetectDriveOffset(OffsetDetectionResult& result);
	bool LookupAccurateRipOffset(DriveOffsetInfo& info);
	bool DetectOffsetFromPregap(int trackStartLBA, int& estimatedOffset);

	// Media control
	bool Eject();

	// Raw SCSI access
	bool SendSCSI(void* cdb, BYTE cdbLength, void* buffer, DWORD bufferSize, bool dataIn = true);

	// Enhanced error handling
	bool SendSCSIWithSense(void* cdb, BYTE cdbLength, void* buffer, DWORD bufferSize,
		BYTE* senseKey, BYTE* asc, BYTE* ascq, bool dataIn = true);
	bool GetMediaStatus(DriveHealthCheck& status);
	bool TestUnitReady();
	bool WaitForDriveReady(int timeoutSeconds = 30);
	std::string GetSenseDescription(BYTE senseKey, BYTE asc, BYTE ascq);

	// Retry configuration
	void SetMaxRetries(int retries) { m_maxRetries = retries; }
	void SetRetryDelay(int delayMs) { m_retryDelayMs = delayMs; }

	// Added method to get actual read and write speeds
	bool GetActualSpeed(WORD& readSpeed, WORD& writeSpeed);

protected:

private:
	bool ReadSectorQRaw(DWORD lba, int& qTrack, int& qIndex);
	bool ParseRawSubchannel(const BYTE* sub, int& qTrack, int& qIndex);

	int m_maxRetries = 5;
	int m_retryDelayMs = 100;
};