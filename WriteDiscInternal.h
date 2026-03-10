#pragma once
#include "AudioCDCopier.h"
#include <string>
#include <vector>
#include <windows.h>

namespace WriteDiscInternal {
	// Drive readiness and cache
	bool WaitForDriveReady(ScsiDrive& drive, int timeoutSeconds);
	bool SynchronizeCache(ScsiDrive& drive);

	// Subchannel helpers
	void DeinterleaveSubchannel(const BYTE* raw, BYTE* packed);
	size_t FindTrackForSector(const std::vector<AudioCDCopier::TrackWriteInfo>& tracks,
		DWORD binSector, bool& isInPregap);

	// Drive configuration
	bool PrepareDriveForWrite(ScsiDrive& drive, int subchannelMode, bool quiet = false);
	bool BuildAndSendCueSheet(ScsiDrive& drive,
		const std::vector<AudioCDCopier::TrackWriteInfo>& tracks,
		DWORD totalSectors, int subchannelMode, bool verbose = true);

	// CD-Text
	bool HasCDTextContent(const std::string& discTitle,
		const std::string& discPerformer,
		const std::vector<AudioCDCopier::TrackWriteInfo>& tracks);

	std::vector<BYTE> BuildCDTextPacks(const std::string& discTitle,
		const std::string& discPerformer,
		const std::vector<AudioCDCopier::TrackWriteInfo>& tracks);

	bool SendCDTextToDevice(ScsiDrive& drive, const std::vector<BYTE>& packs);
}