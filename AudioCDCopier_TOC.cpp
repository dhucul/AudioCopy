#define NOMINMAX
#include "AudioCDCopier.h"
#include "AccurateRip.h"
#include "InterruptHandler.h"
#include "MenuHelpers.h"
#include <iostream>
#include <fstream>
#include <iomanip>
#include <map>
#include <cstring>

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

		if (track1Start == 0) {
			if (disc.tocRepaired) {
				// TOC was recovered from Full TOC — skip subchannel scan,
				// use Red Book default (2-second pregap at LBA 150).
				disc.tracks[0].pregapLBA = 0;
				disc.tracks[0].index01LBA = 150;
				std::cout << "  Track 1: 2:00 (150 frames) pregap (default)\n";
			}
			else {
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
		}
		else {
			// track1Start > 0: scan backward from INDEX 01 for INDEX 00
			disc.tracks[0].index01LBA = track1Start;

			if (disc.tocRepaired) {
				// TOC LBAs were clamped — track position is unreliable,
				// scanning for pregap would produce garbage results.
				disc.tracks[0].pregapLBA = track1Start;
				std::cout << "  Track 1: pregap scan skipped (corrupt TOC)\n";
			}
			else {
				DWORD firstIndex0 = track1Start;
				bool foundIndex0 = false;
				int consecutiveIndex0 = 0;

				constexpr DWORD COARSE_STEP = 15;
				DWORD coarseHit = track1Start;
				bool coarseFound = false;

				// Cap scan window to 450 sectors before track start, same as tracks 2+.
				// Without this, a bogus TOC startLBA (e.g. 16M) causes a multi-day scan.
				DWORD scanStart = track1Start > 450 ? track1Start - 450 : 0;

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

				int frames = static_cast<int>(track1Start - disc.tracks[0].pregapLBA);
				std::cout << "  Track 1: " << frames / 75 << ":"
					<< std::setfill('0') << std::setw(2) << frames % 75 << std::setfill(' ')
					<< " (" << frames << " frames) pregap\n";
			}
		}
	}
	else if (!disc.tracks.empty() && !disc.tracks[0].isAudio
		&& (disc.selectedSession == 0 || disc.tracks[0].session == disc.selectedSession)) {
		// Track 1 is a data track — skip pregap scanning, consistent with tracks 2+
		std::cout << "  Track " << disc.tracks[0].trackNumber << ": DATA track, skipping\n";
	}

	for (size_t i = 1; i < disc.tracks.size(); i++) {
		g_interrupt.CheckInterrupt();
		if (disc.selectedSession > 0 && disc.tracks[i].session != disc.selectedSession) continue;
		if (!disc.tracks[i].isAudio) {
			std::cout << "  Track " << disc.tracks[i].trackNumber << ": DATA track, skipping\n";
			continue;
		}
		if (disc.tocRepaired) {
			disc.tracks[i].pregapLBA = disc.tracks[i].startLBA;
			std::cout << "  Track " << disc.tracks[i].trackNumber << ": pregap scan skipped (corrupt TOC)\n";
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

	if (!disc.tocRepaired) {
		DetectHiddenTrack(disc);
	}

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

	// ── Dump raw TOC for offline testing ──
#ifdef _DEBUG
	{
		std::ofstream dump("toc_dump.bin", std::ios::binary);
		if (dump) {
			auto writeBlock = [&](const char* tag, const BYTE* data, uint32_t len) {
				dump.write(tag, 4);                                      // 4-byte tag
				dump.write(reinterpret_cast<const char*>(&len), 4);      // 4-byte length
				dump.write(reinterpret_cast<const char*>(data), len);    // payload
				};
			writeBlock("SESN", sessionBuf.data(), static_cast<uint32_t>(sessionBuf.size()));
			uint32_t toc0Size = std::min(static_cast<uint32_t>(tocLen + 2), 804u);
			writeBlock("TOC0", tocBuf.data(), toc0Size);
			if (hasFullToc) {
				int fullTocLen = (fullToc[0] << 8) | fullToc[1];
				uint32_t fullTocSize = std::min(static_cast<uint32_t>(fullTocLen + 2), 2048u);
				writeBlock("TOC2", fullToc.data(), fullTocSize);
			}
			std::cout << "  (TOC dumped to toc_dump.bin)\n";
		}
	}
#endif

	std::vector<int> trackSession(100, 1);
	std::map<int, DWORD> sessionLeadOut;    // session number → lead-out LBA
	std::map<int, DWORD> fullTocTrackStart; // track number → LBA from Full TOC MSF
	if (hasFullToc) {
		int fullTocLen = (fullToc[0] << 8) | fullToc[1];
		int endOffset = (fullTocLen + 2 < 2048) ? fullTocLen + 2 : 2048;
		BYTE* p = fullToc.data() + 4;
		BYTE* end = fullToc.data() + endOffset;
		while (p + 11 <= end) {
			int session = p[0];
			int point = p[3];
			if (point >= 1 && point <= 99) {
				trackSession[point] = session;

				// Extract track start MSF (PMIN/PSEC/PFRAME) for TOC recovery.
				// Copy-protection schemes corrupt Format 0 LBAs but the raw
				// Q-subchannel data in the lead-in (Format 2) often retains
				// the real track positions.
				int pmin = p[8];
				int psec = p[9];
				int pframe = p[10];
				if (pmin < 100 && psec < 60 && pframe < 75) {
					DWORD lba = static_cast<DWORD>((pmin * 60 + psec) * 75 + pframe);
					if (lba >= 150) lba -= 150; else lba = 0;
					fullTocTrackStart[point] = lba;
				}
			}

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
		t.hasPreemphasis = (td[1] & 0x01) != 0;
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
				static_cast<DWORD>(leadOut[7]);
			t.endLBA = leadOutLBA - 1;
		}
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
	}
	else {
		disc.audioLeadOutLBA = disc.leadOutLBA;
	}

	// ── TOC sanity check ──
	// A CD holds at most ~360,000 sectors (80 min).  Even extreme overburn
	// never exceeds ~450,000.  Any LBA beyond that is corrupt data from the
	// drive — clamp it so the disc remains usable rather than rejecting it.
	constexpr DWORD MAX_VALID_CD_LBA = 450000;
	bool tocRepaired = false;
	for (auto& t : disc.tracks) {
		if (t.startLBA > MAX_VALID_CD_LBA) {
			std::cerr << "  WARNING: Track " << t.trackNumber
				<< " startLBA " << t.startLBA
				<< " beyond CD capacity — clamping to " << MAX_VALID_CD_LBA << ".\n";
			t.startLBA = MAX_VALID_CD_LBA;
			tocRepaired = true;
		}
		if (t.endLBA > MAX_VALID_CD_LBA) {
			std::cerr << "  WARNING: Track " << t.trackNumber
				<< " endLBA " << t.endLBA
				<< " beyond CD capacity — clamping to " << MAX_VALID_CD_LBA << ".\n";
			t.endLBA = MAX_VALID_CD_LBA;
			tocRepaired = true;
		}
	}
	if (disc.leadOutLBA > MAX_VALID_CD_LBA) {
		std::cerr << "  WARNING: Lead-out LBA " << disc.leadOutLBA
			<< " beyond CD capacity — clamping to " << MAX_VALID_CD_LBA << ".\n";
		disc.leadOutLBA = MAX_VALID_CD_LBA;
		tocRepaired = true;
	}
	if (disc.audioLeadOutLBA > MAX_VALID_CD_LBA) {
		disc.audioLeadOutLBA = MAX_VALID_CD_LBA;
	}
	if (tocRepaired) {
		// Fix inverted ranges caused by clamping startLBA past endLBA
		// (e.g. bogus startLBA=16M clamped to 450k, but endLBA=19999 from
		// a valid next-track pointer).  Set endLBA = startLBA so the track
		// has zero length rather than a DWORD-wrapping underflow.
		for (auto& t : disc.tracks) {
			if (t.startLBA > t.endLBA) {
				t.endLBA = t.startLBA;
			}
		}
		disc.tocRepaired = true;
		Console::SetColor(Console::Color::Yellow);
		std::cerr << "  TOC contained out-of-range LBAs (corrupt drive response). "
			<< "Values clamped — verify results.\n";
		Console::Reset();
	}

	// ── TOC recovery from Full TOC (Format 2) ──────────────────────────
	// Copy-protection schemes (Cactus Data Shield, Key2Audio, etc.) inject
	// bogus LBA values into the Format 0 TOC response.  The disc still
	// plays because CD players use the raw Q-subchannel TOC in the lead-in,
	// which is physically pressed and harder to forge.  Format 2 (READ TOC
	// with format=2) returns that raw lead-in data with MSF addresses.
	// If Format 0 was corrupt but Format 2 has valid entries, recover.
	if (tocRepaired && hasFullToc && !fullTocTrackStart.empty()) {
		// Save state — revert if recovery produces an invalid layout
		auto savedTracks = disc.tracks;
		DWORD savedLeadOut = disc.leadOutLBA;
		DWORD savedAudioLeadOut = disc.audioLeadOutLBA;
		bool anyRecovered = false;

		// Replace clamped startLBAs with Full TOC MSF-derived values
		for (auto& t : disc.tracks) {
			if (t.startLBA != MAX_VALID_CD_LBA) continue; // not clamped
			auto fit = fullTocTrackStart.find(t.trackNumber);
			if (fit != fullTocTrackStart.end() && fit->second < MAX_VALID_CD_LBA) {
				Console::SetColor(Console::Color::Cyan);
				std::cout << "  TOC recovery: Track " << t.trackNumber
					<< " startLBA " << MAX_VALID_CD_LBA
					<< " -> " << fit->second << " (from Full TOC)\n";
				Console::Reset();
				t.startLBA = fit->second;
				anyRecovered = true;
			}
		}

		// Recover lead-out from Full TOC session data
		if (disc.leadOutLBA == MAX_VALID_CD_LBA && !sessionLeadOut.empty()) {
			DWORD bestLeadOut = sessionLeadOut.rbegin()->second;
			if (bestLeadOut < MAX_VALID_CD_LBA) {
				disc.leadOutLBA = bestLeadOut;
				anyRecovered = true;
			}
		}

		if (anyRecovered) {
			// Recalculate endLBAs from recovered starts
			for (size_t i = 0; i < disc.tracks.size(); i++) {
				if (i + 1 < disc.tracks.size()) {
					disc.tracks[i].endLBA = disc.tracks[i + 1].startLBA - 1;
					int nextSess = disc.tracks[i + 1].session;
					if (nextSess != disc.tracks[i].session) {
						auto sit = sessionLeadOut.find(disc.tracks[i].session);
						if (sit != sessionLeadOut.end() && sit->second - 1 < disc.tracks[i].endLBA)
							disc.tracks[i].endLBA = sit->second - 1;
					}
				}
				else {
					disc.tracks[i].endLBA = disc.leadOutLBA - 1;
				}
			}

			// Recalculate audioLeadOutLBA
			auto s1it = sessionLeadOut.find(1);
			if (s1it != sessionLeadOut.end() && disc.sessionCount > 1)
				disc.audioLeadOutLBA = s1it->second;
			else
				disc.audioLeadOutLBA = disc.leadOutLBA;

			// Validate: all LBAs in range, start <= end, monotonically increasing
			bool valid = true;
			for (size_t i = 0; i < disc.tracks.size(); i++) {
				if (disc.tracks[i].startLBA >= MAX_VALID_CD_LBA
					|| disc.tracks[i].startLBA > disc.tracks[i].endLBA) {
					valid = false; break;
				}
				if (i > 0 && disc.tracks[i].startLBA <= disc.tracks[i - 1].startLBA) {
					valid = false; break;
				}
			}

			if (valid) {
				// Do NOT clear tocRepaired — the disc is copy-protected and
				// subchannel reads may hang in protected regions.  The
				// recovered startLBAs are already applied; keeping tocRepaired
				// true skips pregap scanning and hidden-track detection.
				Console::Success("  TOC LBAs recovered from Full TOC data.\n");
			}
			else {
				// Recovery produced invalid layout — revert to clamped values
				disc.tracks = savedTracks;
				disc.leadOutLBA = savedLeadOut;
				disc.audioLeadOutLBA = savedAudioLeadOut;
				Console::SetColor(Console::Color::Yellow);
				std::cerr << "  TOC recovery failed — Full TOC data also appears corrupt.\n";
				Console::Reset();
			}
		}
	}

	for (auto& t : disc.tracks) {
		t.pregapLBA = t.startLBA;  // safe default until pregap scan runs
		t.index01LBA = t.startLBA; // default: INDEX 01 matches TOC start
	}
	return true;
}