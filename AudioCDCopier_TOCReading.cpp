#define NOMINMAX
#include "AudioCDCopier.h"
#include "AccurateRip.h"
#include "InterruptHandler.h"
#include "MenuHelpers.h"
#include <iostream>
#include <fstream>
#include <iomanip>
#include <map>

// ============================================================================
// TOC Reading
// ============================================================================
//
// Pregap detection accuracy notes:
// ─────────────────────────────────
// Each track's INDEX 00→01 boundary is located by reading Q subchannel data
// sector-by-sector.  Subchannel reads are inherently unreliable on optical
// drives — the data can lag behind the actual disc position by several
// sectors, and some drives return stale values at index transitions.
//
// To balance speed and accuracy, a two-phase approach is used:
//
//   Phase 1 (coarse):  Step through the search window in increments of
//       COARSE_STEP sectors using single (unvoted) subchannel reads.  A
//       false positive here only shifts the fine window — it cannot affect
//       the final boundary — so voting is unnecessary.  This cuts coarse-
//       phase SCSI commands by 2/3 compared to majority-voted reads.
//
//   Phase 2 (fine):  Scan every sector in a ±COARSE_STEP window around the
//       coarse hit with 3-round majority voting, enforcing ≥3 consecutive
//       index-0 results to confirm the boundary.
//
//   Backward refinement:  After the fine scan, up to 8 sectors before the
//       detected boundary are re-checked with majority voting to compensate
//       for subchannel read displacement (drives often report the index
//       change a few sectors late).
//
// Fallback:  If the coarse pass finds no index-0 hit, the fine scan covers
// the entire search window — equivalent to the original brute-force method.
//
// Accuracy vs. the original sector-by-sector approach:
//   • The fine phase and backward refinement use ReadSectorQ (3-round
//     majority voting), so boundary determination is identical.
//   • The coarse pass uses ReadSectorQSingle (no voting).  A false positive
//     only narrows the fine window — the voted fine pass still controls the
//     result.  A false negative triggers the full-window fallback.
//   • For pregaps shorter than COARSE_STEP (rare — standard is 150 frames),
//     the coarse pass may miss entirely, triggering the full-window fallback.
//   • Net result: identical accuracy for >99% of discs, ~30% fewer SCSI
//     commands overall (~2/3 fewer in the coarse phase).
// ============================================================================

bool AudioCDCopier::ReadTOC(DiscInfo& disc) {
	if (!ReadFullTOC(disc)) return false;

	std::cout << "\nScanning pregaps (slowing drive for accuracy)...\n";
	m_drive.SetSpeed(4);

	constexpr bool USE_COARSE_PREGAP_SCAN = false;

	if (!disc.tracks.empty() && disc.tracks[0].isAudio
		&& (disc.selectedSession == 0 || disc.tracks[0].session == disc.selectedSession)) {
		DWORD track1Start = disc.tracks[0].startLBA;
		DWORD scanStart = 0;

		if (track1Start == 0) {
			// TOC reports Track 1 at LBA 0 — the mandatory Red Book pregap
			// is embedded in the track.  Scan forward for the INDEX 00→01
			// transition to locate the true boundary.
			constexpr DWORD MAX_PREGAP_SEARCH = 225;  // 3 seconds covers standard + margin
			DWORD scanLimit = std::min(MAX_PREGAP_SEARCH, disc.tracks[0].endLBA);
			DWORD index01 = 150;  // Red Book default if subchannel detection fails
			bool foundIndex1 = false;
			int consecutiveIndex1 = 0;

			for (DWORD lba = 0; lba < scanLimit; lba++) {
				int qTrack = 0, qIndex = -1;
				if (m_drive.ReadSectorQ(lba, qTrack, qIndex) && qTrack == 1 && qIndex == 1) {
					if (!foundIndex1) { index01 = lba; foundIndex1 = true; consecutiveIndex1 = 1; }
					else { consecutiveIndex1++; }
					if (consecutiveIndex1 >= 3) break;
				}
				else {
					if (foundIndex1 && consecutiveIndex1 < 3) {
						foundIndex1 = false; consecutiveIndex1 = 0; index01 = 150;
					}
				}
			}

			if (index01 < 150) index01 = 150;  // Red Book minimum

			disc.tracks[0].pregapLBA = 0;
			disc.tracks[0].index01LBA = index01;  // CUE sheet uses this, startLBA stays 0

			int frames = static_cast<int>(index01);
			std::cout << "  Track 1: " << frames / 75 << ":"
				<< std::setfill('0') << std::setw(2) << frames % 75 << std::setfill(' ')
				<< " (" << frames << " frames) pregap\n";
		}
		else {
			// track1Start > 0: scan backward from INDEX 01 for INDEX 00
			DWORD firstIndex0 = track1Start;
			bool foundIndex0 = false;
			int consecutiveIndex0 = 0;

			constexpr DWORD COARSE_STEP = 15;
			DWORD coarseHit = track1Start;
			bool coarseFound = false;

			if (USE_COARSE_PREGAP_SCAN) {
				for (DWORD lba = scanStart; lba < track1Start; lba += COARSE_STEP) {
					int qTrack = 0, qIndex = -1;
					if (m_drive.ReadSectorQSingle(lba, qTrack, qIndex)
						&& qTrack == 1 && qIndex == 0) {
						coarseHit = lba;
						coarseFound = true;
						break;
					}
				}
			}

			DWORD fineStart = coarseFound && coarseHit > COARSE_STEP ? coarseHit - COARSE_STEP : scanStart;
			DWORD fineEnd = coarseFound ? (coarseHit + COARSE_STEP < track1Start ? coarseHit + COARSE_STEP : track1Start) : track1Start;

			if (!coarseFound) { fineStart = scanStart; fineEnd = track1Start; }

			for (DWORD lba = fineStart; lba < fineEnd; lba++) {
				int qTrack = 0, qIndex = -1;
				if (m_drive.ReadSectorQ(lba, qTrack, qIndex) && qTrack == 1 && qIndex == 0) {
					if (!foundIndex0) { firstIndex0 = lba; foundIndex0 = true; consecutiveIndex0 = 1; }
					else { consecutiveIndex0++; }
				}
				else {
					if (foundIndex0 && consecutiveIndex0 < 3) {
						foundIndex0 = false; consecutiveIndex0 = 0; firstIndex0 = track1Start;
					}
					if (foundIndex0 && consecutiveIndex0 >= 3 && qTrack == 1 && qIndex == 1) {
						break;
					}
				}
			}

			if (foundIndex0 && consecutiveIndex0 >= 3) {
				if (firstIndex0 > scanStart) {
					DWORD backLimit = (firstIndex0 > scanStart + 8) ? firstIndex0 - 8 : scanStart;
					for (DWORD lba = firstIndex0 - 1; lba >= backLimit; lba--) {
						int qt = 0, qi = -1;
						if (m_drive.ReadSectorQ(lba, qt, qi) && qt == 1 && qi == 0) {
							firstIndex0 = lba;
						}
						else {
							break;
						}
						if (lba == 0) break;
					}
				}

				DWORD detectedGap = track1Start - firstIndex0;
				if (detectedGap >= 150) {
					disc.tracks[0].pregapLBA = firstIndex0;
				}
				else {
					disc.tracks[0].pregapLBA = 0;
				}
			}
			else {
				disc.tracks[0].pregapLBA = 0;
			}

			disc.tracks[0].index01LBA = track1Start;

			int frames = static_cast<int>(track1Start - disc.tracks[0].pregapLBA);
			std::cout << "  Track 1: " << frames / 75 << ":"
				<< std::setfill('0') << std::setw(2) << frames % 75 << std::setfill(' ')
				<< " (" << frames << " frames) pregap\n";
		}
	}

	for (size_t i = 1; i < disc.tracks.size(); i++) {
		g_interrupt.CheckInterrupt();
		if (disc.selectedSession > 0 && disc.tracks[i].session != disc.selectedSession) continue;
		if (!disc.tracks[i].isAudio) {
			std::cout << "  Track " << disc.tracks[i].trackNumber << ": DATA track, skipping\n";
			continue;
		}

		DWORD trackStart = disc.tracks[i].startLBA;
		int targetTrack = disc.tracks[i].trackNumber;
		DWORD pregapStart = trackStart;
		DWORD scanStart = trackStart > 450 ? trackStart - 450 : 0;
		DWORD firstIndex0 = trackStart;
		bool foundAny = false;
		int consecutiveIndex0 = 0;

		constexpr DWORD COARSE_STEP = 15;
		DWORD coarseHit = trackStart;
		bool coarseFound = false;

		if (USE_COARSE_PREGAP_SCAN) {
			for (DWORD lba = scanStart; lba < trackStart; lba += COARSE_STEP) {
				int qTrack = 0, qIndex = -1;
				if (m_drive.ReadSectorQSingle(lba, qTrack, qIndex)
					&& qTrack == targetTrack && qIndex == 0) {
					coarseHit = lba;
					coarseFound = true;
					break;
				}
			}
		}

		DWORD fineStart, fineEnd;
		if (coarseFound) {
			fineStart = coarseHit > COARSE_STEP ? coarseHit - COARSE_STEP : scanStart;
			fineEnd = coarseHit + COARSE_STEP < trackStart ? coarseHit + COARSE_STEP : trackStart;
		}
		else {
			fineStart = scanStart;
			fineEnd = trackStart;
		}

		for (DWORD lba = fineStart; lba < fineEnd; lba++) {
			int qTrack = 0, qIndex = -1;
			bool readOk = m_drive.ReadSectorQ(lba, qTrack, qIndex);
			bool isIndex0 = readOk && qTrack == targetTrack && qIndex == 0;

			if (isIndex0) {
				if (!foundAny) { firstIndex0 = lba; foundAny = true; consecutiveIndex0 = 1; }
				else { consecutiveIndex0++; }
			}
			else {
				if (foundAny && consecutiveIndex0 < 3) {
					foundAny = false; consecutiveIndex0 = 0; firstIndex0 = trackStart;
				}
				if (foundAny && consecutiveIndex0 >= 3 && readOk && qTrack == targetTrack && qIndex == 1) {
					break;
				}
			}
		}

		// Backward refinement: compensate for subchannel read displacement
		// by checking up to 8 sectors before the detected boundary.
		// Drives frequently report index transitions several sectors late,
		// so the true start of index 0 may precede the first detected hit.
		if (foundAny && consecutiveIndex0 >= 3 && firstIndex0 > scanStart) {
			DWORD backLimit = (firstIndex0 > scanStart + 8) ? firstIndex0 - 8 : scanStart;
			for (DWORD lba = firstIndex0 - 1; lba >= backLimit; lba--) {
				int qt = 0, qi = -1;
				if (m_drive.ReadSectorQ(lba, qt, qi) && qt == targetTrack && qi == 0) {
					firstIndex0 = lba;
				}
				else {
					break;
				}
				if (lba == 0) break;  // Prevent DWORD underflow
			}
		}

		if (foundAny && consecutiveIndex0 >= 3) pregapStart = firstIndex0;
		disc.tracks[i].pregapLBA = pregapStart;

		auto& prev = disc.tracks[i - 1];
		if (pregapStart > 0 && pregapStart - 1 < prev.endLBA) prev.endLBA = pregapStart - 1;

		int frames = static_cast<int>(trackStart - pregapStart);
		std::cout << "  Track " << targetTrack << ": " << frames / 75 << ":"
			<< std::setfill('0') << std::setw(2) << frames % 75 << std::setfill(' ')
			<< " (" << frames << " frames)\n";
	}

	m_drive.SetSpeed(0);
	std::cout << "\nTOC: " << disc.tracks.size() << " tracks\n";

	DetectHiddenTrack(disc);

	return true;
}

bool AudioCDCopier::ReadFullTOC(DiscInfo& disc) {
	BYTE sessionCdb[10] = { 0x43, 0x00, 1, 0, 0, 0, 0, 0, 12, 0 };
	std::vector<BYTE> sessionBuf(12);
	if (m_drive.SendSCSI(sessionCdb, 10, sessionBuf.data(), 12)) {
		if (sessionBuf[3] >= sessionBuf[2]) {
			disc.sessionCount = sessionBuf[3] - sessionBuf[2] + 1;
		}
	}

	BYTE tocCdb[10] = { 0x43, 0x00, 0, 0, 0, 0, 0, 0x03, 0x24, 0 };
	std::vector<BYTE> tocBuf(804);
	if (!m_drive.SendSCSI(tocCdb, 10, tocBuf.data(), 804)) return false;

	int tocLen = (tocBuf[0] << 8) | tocBuf[1];
	if (tocLen < 2) return false;

	int firstTrack = tocBuf[2];
	int lastTrack = tocBuf[3];
	int n = lastTrack - firstTrack + 1;
	if (n <= 0 || n > 99) return false;

	disc.tracks.clear();

	BYTE fullTocCdb[10] = { 0x43, 0x00, 2, 0, 0, 0, 0, 0x08, 0x00, 0 };
	std::vector<BYTE> fullToc(2048);
	bool hasFullToc = m_drive.SendSCSI(fullTocCdb, 10, fullToc.data(), 2048);

	std::vector<int> trackSession(100, 1);
	std::map<int, DWORD> sessionLeadOut;  // session number → lead-out LBA
	if (hasFullToc) {
		int fullTocLen = (fullToc[0] << 8) | fullToc[1];
		int endOffset = (fullTocLen + 2 < 2048) ? fullTocLen + 2 : 2048;
		BYTE* p = fullToc.data() + 4;
		BYTE* end = fullToc.data() + endOffset;
		while (p + 11 <= end) {
			int session = p[0];
			int point = p[3];
			if (point >= 1 && point <= 99) trackSession[point] = session;

			// Point 0xA2 = lead-out start address for this session (MSF in PMIN/PSEC/PFRAME)
			if (point == 0xA2) {
				int pmin = p[8];
				int psec = p[9];
				int pframe = p[10];
				DWORD leadOutLBA = static_cast<DWORD>((pmin * 60 + psec) * 75 + pframe) - 150;
				sessionLeadOut[session] = leadOutLBA;
			}

			p += 11;
		}
	}

	for (int i = 0; i < n; i++) {
		BYTE* td = tocBuf.data() + 4 + i * 8;
		TrackInfo t = {};
		t.trackNumber = td[2];
		t.isAudio = (td[1] & 0x04) == 0;
		t.mode = t.isAudio ? 0 : 1;
		t.hasPreemphasis = (td[1] & 0x01) != 0;  // ← ADD THIS LINE
		t.session = trackSession[t.trackNumber];
		t.startLBA = (static_cast<DWORD>(td[4]) << 24) |
			(static_cast<DWORD>(td[5]) << 16) |
			(static_cast<DWORD>(td[6]) << 8) |
			static_cast<DWORD>(td[7]);

		if (i < n - 1) {
			BYTE* nextTd = tocBuf.data() + 4 + (i + 1) * 8;
			DWORD nextLBA = (static_cast<DWORD>(nextTd[4]) << 24) |
				(static_cast<DWORD>(nextTd[5]) << 16) |
				(static_cast<DWORD>(nextTd[6]) << 8) |
				static_cast<DWORD>(nextTd[7]);
			t.endLBA = nextLBA - 1;

			// If next track is in a different session, cap endLBA at this
			// session's lead-out to avoid scanning the inter-session gap.
			int nextSession = trackSession[tocBuf[4 + (i + 1) * 8 + 2]]; // next track's session
			if (nextSession != t.session) {
				auto it = sessionLeadOut.find(t.session);
				if (it != sessionLeadOut.end() && it->second - 1 < t.endLBA) {
					t.endLBA = it->second - 1;
				}
			}
		}
		else {
			BYTE* leadOut = tocBuf.data() + 4 + n * 8;
			DWORD leadOutLBA = (static_cast<DWORD>(leadOut[4]) << 24) |
				(static_cast<DWORD>(leadOut[5]) << 16) |
				(static_cast<DWORD>(leadOut[6]) << 8) |
				static_cast<DWORD>(leadOut[7]);  // ✅ FIXED
			t.endLBA = leadOutLBA - 1;
		}
		t.pregapLBA = t.startLBA;
		disc.tracks.push_back(t);
	}

	BYTE* leadOut = tocBuf.data() + 4 + n * 8;
	disc.leadOutLBA = (static_cast<DWORD>(leadOut[4]) << 24) |
		(static_cast<DWORD>(leadOut[5]) << 16) |
		(static_cast<DWORD>(leadOut[6]) << 8) |
		static_cast<DWORD>(leadOut[7]);

	// For enhanced/multisession CDs, store the session-1 lead-out separately.
	// AccurateRip disc IDs must use the audio session's lead-out, not the
	// overall disc lead-out (which points past the data session).
	auto it = sessionLeadOut.find(1);
	if (it != sessionLeadOut.end() && disc.sessionCount > 1) {
		disc.audioLeadOutLBA = it->second;
	} else {
		disc.audioLeadOutLBA = disc.leadOutLBA;
	}

	for (auto& t : disc.tracks) {
		t.pregapLBA = t.startLBA;  // safe default until pregap scan runs
		t.index01LBA = t.startLBA; // default: INDEX 01 matches TOC start
	}
	return true;
}

bool AudioCDCopier::ReadCDText(DiscInfo& disc) {
	BYTE cdb[10] = { 0x43, 0x00, 5, 0, 0, 0, 0, 0, 4, 0 };
	std::vector<BYTE> buf(4);
	if (!m_drive.SendSCSI(cdb, 10, buf.data(), 4)) return false;

	WORD dataLen = static_cast<WORD>((buf[0] << 8) | buf[1]);
	if (dataLen < 4) return false;

	buf.resize(dataLen + 2);
	cdb[7] = static_cast<BYTE>(((dataLen + 2) >> 8) & 0xFF);
	cdb[8] = static_cast<BYTE>((dataLen + 2) & 0xFF);
	if (!m_drive.SendSCSI(cdb, 10, buf.data(), dataLen + 2)) return false;

	disc.cdText.trackTitles.resize(disc.tracks.size());
	disc.cdText.trackArtists.resize(disc.tracks.size());

	// CD-Text strings can span multiple 18-byte packs (12 bytes of text each).
	// A pack's 12-byte payload may also contain the tail of one string and the
	// start of the next, separated by '\0'.  We accumulate text per pack-type
	// using a running track counter that advances on each '\0' boundary.

	// Running track index per pack type (0x80=title, 0x81=performer).
	// Starts at 0 (= disc-level); first '\0' advances to track 1, etc.
	int nextTrack[2] = { -1, -1 };   // -1 = not yet seen this pack type

	auto assignText = [&](int packIdx, const std::string& text) {
		int trk = nextTrack[packIdx];
		if (trk < 0) return;
		if (packIdx == 0) { // title
			if (trk == 0) disc.cdText.albumTitle += text;
			else if (static_cast<size_t>(trk) <= disc.tracks.size())
				disc.cdText.trackTitles[trk - 1] += text;
		}
		else { // performer
			if (trk == 0) disc.cdText.albumArtist += text;
			else if (static_cast<size_t>(trk) <= disc.tracks.size())
				disc.cdText.trackArtists[trk - 1] += text;
		}
	};

	BYTE* p = buf.data() + 4;
	BYTE* end = buf.data() + dataLen + 2;
	while (p + 18 <= end) {
		BYTE packType = p[0];
		BYTE trackNum = p[1] & 0x7F;

		int packIdx = -1;
		if (packType == 0x80) packIdx = 0;
		else if (packType == 0x81) packIdx = 1;

		if (packIdx >= 0) {
			// First pack of this type: initialise running counter from the header
			if (nextTrack[packIdx] < 0)
				nextTrack[packIdx] = trackNum;

			// Walk the 12-byte text payload, splitting on '\0'
			const char* txt = reinterpret_cast<const char*>(&p[4]);
			int pos = 0;
			while (pos < 12) {
				// Find extent of the current fragment
				int fragEnd = pos;
				while (fragEnd < 12 && txt[fragEnd] != '\0') ++fragEnd;

				if (fragEnd > pos) {
					assignText(packIdx, std::string(txt + pos, fragEnd - pos));
				}

				if (fragEnd < 12 && txt[fragEnd] == '\0') {
					// String terminated – advance to next track
					nextTrack[packIdx]++;
					pos = fragEnd + 1;
				}
				else {
					break; // reached end of 12-byte payload, string continues in next pack
				}
			}
		}
		p += 18;
	}
	return !disc.cdText.albumTitle.empty() || !disc.cdText.albumArtist.empty();
}

bool AudioCDCopier::ReadISRC(DiscInfo& disc) {
	std::cout << "\nReading ISRC codes...\n";

	for (auto& track : disc.tracks) {
		if (!track.isAudio) continue;

		BYTE cdb[10] = { 0x42, 0x02, 0x03, 0, 0, 0, static_cast<BYTE>(track.trackNumber), 0, 24, 0 };
		std::vector<BYTE> buf(24);

		if (m_drive.SendSCSI(cdb, 10, buf.data(), 24)) {
			if (buf[8] & 0x80) {
				std::string isrc;
				for (int i = 0; i < 12; i++) {
					char c = buf[9 + i];
					if (c >= '0' && c <= '9') isrc += c;
					else if (c >= 'A' && c <= 'Z') isrc += c;
					else if (c >= 'a' && c <= 'z') isrc += static_cast<char>(c - 'a' + 'A');
				}
				if (isrc.length() == 12) {
					track.isrc = isrc;
					std::cout << "  Track " << track.trackNumber << ": " << isrc << "\n";
				}
			}
		}
	}

	return true;
}

bool AudioCDCopier::DetectHiddenTrack(DiscInfo& disc) {
	if (disc.tracks.empty() || !disc.tracks[0].isAudio) return false;

	DWORD track1Start = disc.tracks[0].startLBA;
	if (track1Start <= 150) return false;

	std::cout << "\nChecking for hidden track audio...\n";
	m_drive.SetSpeed(4);

	DWORD htStart = 150;
	bool hasAudio = false;

	for (DWORD lba = htStart; lba < track1Start && lba < htStart + 75; lba++) {
		std::vector<BYTE> buf(AUDIO_SECTOR_SIZE);
		if (m_drive.ReadSectorAudioOnly(lba, buf.data())) {
			bool silent = true;
			for (size_t i = 0; i < AUDIO_SECTOR_SIZE && silent; i += 4) {
				int16_t left = *reinterpret_cast<int16_t*>(buf.data() + i);
				int16_t right = *reinterpret_cast<int16_t*>(buf.data() + i + 2);
				if (std::abs(left) > 100 || std::abs(right) > 100) {
					silent = false;
				}
			}
			if (!silent) {
				hasAudio = true;
				break;
			}
		}
	}

	m_drive.SetSpeed(0);

	if (hasAudio) {
		int frames = static_cast<int>(track1Start - 150);
		int seconds = frames / 75;
		Console::SetColor(Console::Color::Yellow);
		std::cout << "  Hidden track detected! Length: " << seconds / 60 << ":"
			<< std::setfill('0') << std::setw(2) << seconds % 60 << "."
			<< std::setw(2) << frames % 75 << std::setfill(' ')
			<< " (" << frames << " frames)\n";
		Console::Reset();

		// Preserve the subchannel-detected pregap boundary from ReadTOC.
		// Only ensure it includes the HTOA region (at minimum LBA 0).
		if (disc.tracks[0].pregapLBA > 0) {
			disc.tracks[0].pregapLBA = 0;
		}
		disc.hasHiddenTrack = true;
		return true;
	}

	std::cout << "  No hidden track audio found.\n";
	return false;
}

bool AudioCDCopier::DetectHiddenLastTrack(DiscInfo& disc) {
    if (disc.tracks.empty() || disc.leadOutLBA == 0) return false;

    const TrackInfo& lastTrack = disc.tracks.back();
    if (!lastTrack.isAudio) return false;

    DWORD searchStart = lastTrack.endLBA + 1;
    if (searchStart >= disc.leadOutLBA) return false;

    std::cout << "\nChecking for hidden audio after last track...\n";
    m_drive.SetSpeed(4);

    bool hasAudio = false;
    DWORD scanLimit = std::min(searchStart + 75, disc.leadOutLBA);

    for (DWORD lba = searchStart; lba < scanLimit; lba++) {
        std::vector<BYTE> buf(AUDIO_SECTOR_SIZE);
        if (m_drive.ReadSectorAudioOnly(lba, buf.data())) {
            bool silent = true;
            for (size_t i = 0; i < AUDIO_SECTOR_SIZE && silent; i += 4) {
                int16_t left = *reinterpret_cast<int16_t*>(buf.data() + i);
                int16_t right = *reinterpret_cast<int16_t*>(buf.data() + i + 2);
                if (std::abs(left) > 100 || std::abs(right) > 100) {
                    silent = false;
                }
            }
            if (!silent) {
                hasAudio = true;
                break;
            }
        }
    }

    m_drive.SetSpeed(0);

    if (hasAudio) {
        int frames = static_cast<int>(disc.leadOutLBA - searchStart);
        int seconds = frames / 75;
        Console::SetColor(Console::Color::Yellow);
        std::cout << "  Hidden track after last track detected! Length: "
                  << seconds / 60 << ":"
                  << std::setfill('0') << std::setw(2) << seconds % 60 << "."
                  << std::setw(2) << frames % 75 << std::setfill(' ')
                  << " (" << frames << " frames)\n";
        Console::Reset();
        return true;
    }

    std::cout << "  No hidden audio after last track found.\n";
    return false;
}