#define NOMINMAX
#include "WriteDiscInternal.h"
#include "ConsoleColors.h"
#include <cstring>
#include <vector>

// ============================================================================
// Helper: Calculate CRC-16 for CD-Text packs (CRC-CCITT, poly 0x1021)
// ============================================================================
static uint16_t CDTextCRC(const BYTE* data, int len) {
	uint16_t crc = 0;
	for (int i = 0; i < len; i++) {
		crc ^= static_cast<uint16_t>(data[i]) << 8;
		for (int b = 0; b < 8; b++) {
			if (crc & 0x8000)
				crc = (crc << 1) ^ 0x1021;
			else
				crc <<= 1;
		}
	}
	return ~crc;
}

// ============================================================================
// Helper: Build CD-Text packs from CUE metadata
// ============================================================================
std::vector<BYTE> WriteDiscInternal::BuildCDTextPacks(
	const std::string& discTitle,
	const std::string& discPerformer,
	const std::vector<AudioCDCopier::TrackWriteInfo>& tracks) {

	std::vector<BYTE> packs;

	auto buildPacksForType = [&](BYTE packType,
		const std::string& discStr,
		const std::vector<std::string>& trackStrs) {

			std::vector<BYTE> textBuf;
			std::vector<BYTE> trackNumAtString;

			trackNumAtString.push_back(0x00);
			for (char c : discStr) textBuf.push_back(static_cast<BYTE>(c));
			textBuf.push_back(0x00);

			for (size_t ti = 0; ti < trackStrs.size(); ti++) {
				BYTE tno = static_cast<BYTE>(tracks[ti].trackNumber);
				trackNumAtString.push_back(tno);
				for (char c : trackStrs[ti]) textBuf.push_back(static_cast<BYTE>(c));
				textBuf.push_back(0x00);
			}

			BYTE seqBase = static_cast<BYTE>(packs.size() / 18);
			size_t offset = 0;
			int stringIdx = 0;
			int charInString = 0;

			while (offset < textBuf.size()) {
				BYTE pack[18] = { 0 };
				pack[0] = packType;
				pack[1] = trackNumAtString[stringIdx];
				pack[2] = seqBase++;
				pack[3] = static_cast<BYTE>(charInString);

				for (int i = 0; i < 12 && offset < textBuf.size(); i++, offset++) {
					pack[4 + i] = textBuf[offset];
					if (textBuf[offset] == 0x00) {
						stringIdx++;
						charInString = 0;
					}
					else {
						charInString++;
					}
				}

				uint16_t crc = CDTextCRC(pack, 16);
				pack[16] = static_cast<BYTE>((crc >> 8) & 0xFF);
				pack[17] = static_cast<BYTE>(crc & 0xFF);

				packs.insert(packs.end(), pack, pack + 18);
			}
		};

	std::vector<std::string> trackTitles, trackPerformers;
	for (const auto& t : tracks) {
		trackTitles.push_back(t.title.empty() ? "" : t.title);
		trackPerformers.push_back(t.performer.empty() ? "" : t.performer);
	}

	buildPacksForType(0x80, discTitle, trackTitles);
	buildPacksForType(0x81, discPerformer, trackPerformers);

	{
		int totalPacks = static_cast<int>(packs.size() / 18);
		int titlePacks = 0, performerPacks = 0;
		for (int i = 0; i < totalPacks; i++) {
			BYTE pt = packs[i * 18];
			if (pt == 0x80) titlePacks++;
			else if (pt == 0x81) performerPacks++;
		}

		BYTE firstTrack = static_cast<BYTE>(tracks.empty() ? 1 : tracks.front().trackNumber);
		BYTE lastTrack = static_cast<BYTE>(tracks.empty() ? 0 : tracks.back().trackNumber);

		BYTE sizeInfo[36] = { 0 };
		sizeInfo[0] = 0x00;
		sizeInfo[1] = firstTrack;
		sizeInfo[2] = lastTrack;
		sizeInfo[3] = 0x00;
		sizeInfo[4] = static_cast<BYTE>(titlePacks);
		sizeInfo[5] = static_cast<BYTE>(performerPacks);
		sizeInfo[15] = 3;
		sizeInfo[16] = static_cast<BYTE>(totalPacks + 2);
		sizeInfo[24] = 0x09;

		BYTE seqBase = static_cast<BYTE>(packs.size() / 18);
		for (int p = 0; p < 3; p++) {
			BYTE pack[18] = { 0 };
			pack[0] = 0x8F;
			pack[1] = static_cast<BYTE>(p);
			pack[2] = seqBase++;
			pack[3] = 0x00;
			memcpy(&pack[4], &sizeInfo[p * 12], 12);

			uint16_t crc = CDTextCRC(pack, 16);
			pack[16] = static_cast<BYTE>((crc >> 8) & 0xFF);
			pack[17] = static_cast<BYTE>(crc & 0xFF);

			packs.insert(packs.end(), pack, pack + 18);
		}
	}

	return packs;
}

// ============================================================================
// Helper: Check if any CD-Text content exists
// ============================================================================
bool WriteDiscInternal::HasCDTextContent(const std::string& discTitle,
	const std::string& discPerformer,
	const std::vector<AudioCDCopier::TrackWriteInfo>& tracks) {
	if (!discTitle.empty() || !discPerformer.empty()) return true;
	for (const auto& t : tracks) {
		if (!t.title.empty() || !t.performer.empty()) return true;
	}
	return false;
}

// ============================================================================
// Helper: Send CD-Text packs to drive via WRITE BUFFER (0x3B) buffer ID 0x08
// ============================================================================
bool WriteDiscInternal::SendCDTextToDevice(ScsiDrive& drive, const std::vector<BYTE>& packs) {
	if (packs.empty()) return false;

	DWORD packDataLen = static_cast<DWORD>(packs.size());
	DWORD totalLen = 4 + packDataLen;
	std::vector<BYTE> buf(totalLen, 0);

	WORD dataLen = static_cast<WORD>(totalLen - 2);
	buf[0] = static_cast<BYTE>((dataLen >> 8) & 0xFF);
	buf[1] = static_cast<BYTE>(dataLen & 0xFF);
	memcpy(buf.data() + 4, packs.data(), packDataLen);

	BYTE cdb[10] = { 0x3B, 0x02, 0x08, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };
	cdb[6] = static_cast<BYTE>((totalLen >> 16) & 0xFF);
	cdb[7] = static_cast<BYTE>((totalLen >> 8) & 0xFF);
	cdb[8] = static_cast<BYTE>(totalLen & 0xFF);

	BYTE senseKey = 0, asc = 0, ascq = 0;
	if (drive.SendSCSIWithSense(cdb, sizeof(cdb), buf.data(), totalLen,
		&senseKey, &asc, &ascq, false)) {
		Console::Success("CD-Text sent via WRITE BUFFER (");
		std::cout << (packDataLen / 18) << " packs)\n";
		return true;
	}

	Console::Warning("CD-Text WRITE BUFFER failed (");
	std::cout << drive.GetSenseDescription(senseKey, asc, ascq) << ")\n";
	return false;
}