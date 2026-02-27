#pragma once
#include "AudioCDCopier.h"
#include <string>
#include <vector>
#include <windows.h>

namespace WriteDiscInternal {
	bool PrepareDriveForWrite(ScsiDrive& drive, int subchannelMode);
	bool BuildAndSendCueSheet(ScsiDrive& drive,
		const std::vector<AudioCDCopier::TrackWriteInfo>& tracks,
		DWORD totalSectors, int subchannelMode);

	bool HasCDTextContent(const std::string& discTitle,
		const std::string& discPerformer,
		const std::vector<AudioCDCopier::TrackWriteInfo>& tracks);

	std::vector<BYTE> BuildCDTextPacks(const std::string& discTitle,
		const std::string& discPerformer,
		const std::vector<AudioCDCopier::TrackWriteInfo>& tracks);

	bool SendCDTextToDevice(ScsiDrive& drive, const std::vector<BYTE>& packs);
}