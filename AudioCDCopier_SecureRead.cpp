#define NOMINMAX
#include "AudioCDCopier.h"
#include "InterruptHandler.h"
#include "MenuHelpers.h"
#include <iostream>
#include <iomanip>
#include <chrono>
#include <algorithm>
#include <map>
#include <cstring>

// ============================================================================
// Secure Rip Mode
// ============================================================================

bool AudioCDCopier::ReadDiscSecure(DiscInfo& disc, const SecureRipConfig& config,
	SecureRipResult& result, std::function<void(int, int)> progress) {

	SecureRipConfig effectiveConfig = config;
	effectiveConfig.cacheDefeat = disc.enableCacheDefeat;
	if (!disc.enableC2Detection) effectiveConfig.useC2 = false;

	DWORD total = 0;
	for (size_t i = 0; i < disc.tracks.size(); i++) {
		if (disc.selectedSession > 0 && disc.tracks[i].session != disc.selectedSession) continue;
		DWORD start = (disc.pregapMode == PregapMode::Skip) ? disc.tracks[i].startLBA : disc.tracks[i].pregapLBA;
		total += disc.tracks[i].endLBA - start + 1;
	}

	result = SecureRipResult{};
	result.totalSectors = static_cast<int>(total);

	try {
		disc.rawSectors.clear();
		disc.rawSectors.reserve(total);
	}
	catch (const std::bad_alloc&) {
		std::cerr << "Error: Not enough memory\n";
		return false;
	}

	disc.errorCount = 0;
	disc.badSectors.clear();

	bool trustC2Clean = effectiveConfig.useC2 && effectiveConfig.c2Guided;

	// Initialize log with configuration
	auto& log = result.log;
	log.modeName = (effectiveConfig.mode == SecureRipMode::Fast) ? "Fast" :
		(effectiveConfig.mode == SecureRipMode::Standard) ? "Standard" :
		(effectiveConfig.mode == SecureRipMode::Paranoid) ? "Paranoid" : "Custom";
	log.minPasses = effectiveConfig.minPasses;
	log.maxPasses = effectiveConfig.maxPasses;
	log.requiredMatches = effectiveConfig.requiredMatches;
	log.useC2 = effectiveConfig.useC2;
	log.cacheDefeat = effectiveConfig.cacheDefeat;
	log.totalSectors = static_cast<int>(total);

	bool logToFile = (disc.loggingOutput == LogOutput::File || disc.loggingOutput == LogOutput::Both);
	if (logToFile) log.entries.reserve(total);

	std::cout << "  Secure rip: " << effectiveConfig.minPasses << "-" << effectiveConfig.maxPasses
		<< " passes, require " << effectiveConfig.requiredMatches << " matches\n";
	std::cout << "  Cache defeat: " << (effectiveConfig.cacheDefeat ? "ENABLED" : "DISABLED") << "\n";
	std::cout << "  C2-guided: " << (trustC2Clean ? "YES (fast path for clean sectors)" : "NO (all sectors verified)") << "\n";
	std::cout << "  (Press ESC or Ctrl+C to cancel)\n" << std::flush;

	if (progress) progress(0, total);

	auto overallStart = std::chrono::steady_clock::now();

	// ========================================================================
	// PHASE 1: Fast first pass — read everything, hash each sector
	// ========================================================================
	struct SectorState {
		size_t index;
		int sectorSize;
		uint32_t hash;
		int matchCount;
		bool isAudio;
		int track;
		bool hadC2Errors;
	};

	std::vector<DWORD> rereadLBAs;
	std::map<DWORD, SectorState> sectorStates;

	auto phase1Start = std::chrono::steady_clock::now();
	SecureRipPhaseStats phase1Stats{ 1, 0, 0, 0, 0.0, 0.0 };
	double phase1TotalReadTime = 0.0;

	DWORD cur = 0;
	for (size_t i = 0; i < disc.tracks.size(); i++) {
		auto& t = disc.tracks[i];
		if (disc.selectedSession > 0 && t.session != disc.selectedSession) continue;
		DWORD start = (disc.pregapMode == PregapMode::Skip) ? t.startLBA : t.pregapLBA;
		int sectorSize = (disc.includeSubchannel && t.isAudio) ? RAW_SECTOR_SIZE : AUDIO_SECTOR_SIZE;

		for (DWORD lba = start; lba <= t.endLBA; lba++) {
			if (g_interrupt.IsInterrupted() || g_interrupt.CheckEscapeKey()) {
				return false;
			}

			std::vector<BYTE> sec(sectorSize, 0);
			int c2Errors = 0;
			bool ok = false;

			auto sectorStart = std::chrono::steady_clock::now();

			if (t.isAudio && effectiveConfig.useC2) {
				ScsiDrive::C2ReadOptions c2Opts;
				c2Opts.countBytes = true;
				BYTE* subPtr = (sectorSize > AUDIO_SECTOR_SIZE) ? sec.data() + AUDIO_SECTOR_SIZE : nullptr;
				ok = m_drive.ReadSectorWithC2Ex(lba, sec.data(), subPtr, c2Errors, nullptr, c2Opts);
			}
			else if (t.isAudio) {
				if (sectorSize > AUDIO_SECTOR_SIZE)
					ok = m_drive.ReadSector(lba, sec.data(), sec.data() + AUDIO_SECTOR_SIZE);
				else
					ok = m_drive.ReadSectorAudioOnly(lba, sec.data());
			}
			else {
				ok = m_drive.ReadDataSector(lba, sec.data());
			}

			double readTimeMs = std::chrono::duration<double, std::milli>(
				std::chrono::steady_clock::now() - sectorStart).count();
			phase1TotalReadTime += readTimeMs;

			size_t idx = disc.rawSectors.size();
			uint32_t hash = ok ? HashSector(sec.data(), AUDIO_SECTOR_SIZE) : 0;
			disc.rawSectors.push_back(std::move(sec));

			bool phase1Trusted = ok && (c2Errors == 0);
			sectorStates[lba] = { idx, sectorSize, hash, phase1Trusted ? 1 : 0,
				t.isAudio, t.trackNumber, (c2Errors > 0) };
			phase1Stats.sectorsProcessed++;

			bool verified = false;
			if (!ok || c2Errors > 0) {
				rereadLBAs.push_back(lba);
				log.totalC2Errors += c2Errors;
			}
			else if (trustC2Clean || !t.isAudio) {
				result.secureSectors++;
				result.singlePassSectors++;
				phase1Stats.sectorsVerified++;
				verified = true;
			}
			else {
				rereadLBAs.push_back(lba);
			}

			if (logToFile) {
				log.entries.push_back({ lba, t.trackNumber, 1, 1, ok ? 1 : 0,
					c2Errors, readTimeMs, verified, hash });
			}

			cur++;
			if (progress) progress(cur, total);
		}
	}

	phase1Stats.durationSeconds = std::chrono::duration<double>(
		std::chrono::steady_clock::now() - phase1Start).count();
	phase1Stats.avgReadTimeMs = phase1Stats.sectorsProcessed > 0
		? phase1TotalReadTime / phase1Stats.sectorsProcessed : 0.0;
	phase1Stats.sectorsFailed = static_cast<int>(rereadLBAs.size());
	log.phaseStats.push_back(phase1Stats);

	if (rereadLBAs.empty()) {
		log.totalVerified = result.secureSectors;
		log.totalDurationSeconds = phase1Stats.durationSeconds;
		result.securityConfidence = 100.0;
		result.qualityAssessment = "Excellent";
		std::cout << "\n  All sectors verified — no re-reads needed\n";
		std::cout << "\n  Secure: " << result.secureSectors << "/" << result.totalSectors
			<< " (" << std::fixed << std::setprecision(1) << result.securityConfidence << "%)\n";
		return true;
	}

	std::sort(rereadLBAs.begin(), rereadLBAs.end());

	// ========================================================================
	// PHASE 2: Sequential sweep re-reads (no per-sector cache defeat)
	// ========================================================================
	std::vector<DWORD> stillUnverified;
	int maxSweeps = effectiveConfig.maxPasses - 1;
	int totalPhase2Verified = 0;
	auto phase2Start = std::chrono::steady_clock::now();
	double phase2TotalReadTime = 0.0;
	int phase2TotalProcessed = 0;

	for (int sweep = 0; sweep < maxSweeps && !rereadLBAs.empty(); sweep++) {
		std::cout << "\n  Phase 2 sweep " << (sweep + 1) << "/" << maxSweeps << ": "
			<< rereadLBAs.size() << " sectors\n";

		ProgressIndicator sweepProgress;
		sweepProgress.SetLabel("  Verify");
		sweepProgress.Start();

		int sweepTotal = static_cast<int>(rereadLBAs.size());
		int sweepCur = 0;
		stillUnverified.clear();

		if (sweep == 0 && !rereadLBAs.empty()) {
			DefeatDriveCache(rereadLBAs.front(), disc.leadOutLBA);
		}

		for (DWORD lba : rereadLBAs) {
			if (g_interrupt.IsInterrupted() || g_interrupt.CheckEscapeKey()) {
				sweepProgress.Finish(false, sweepTotal);
				return false;
			}

			auto& state = sectorStates[lba];
			std::vector<BYTE> buf(state.sectorSize, 0);
			bool ok = false;
			int c2Errors = 0;

			auto sectorStart = std::chrono::steady_clock::now();

			if (state.isAudio && effectiveConfig.useC2) {
				ScsiDrive::C2ReadOptions c2Opts;
				c2Opts.countBytes = true;
				BYTE* subPtr = (state.sectorSize > AUDIO_SECTOR_SIZE) ? buf.data() + AUDIO_SECTOR_SIZE : nullptr;
				ok = m_drive.ReadSectorWithC2Ex(lba, buf.data(), subPtr, c2Errors, nullptr, c2Opts);
			}
			else if (state.isAudio) {
				if (state.sectorSize > AUDIO_SECTOR_SIZE)
					ok = m_drive.ReadSector(lba, buf.data(), buf.data() + AUDIO_SECTOR_SIZE);
				else
					ok = m_drive.ReadSectorAudioOnly(lba, buf.data());
			}
			else {
				ok = m_drive.ReadDataSector(lba, buf.data());
			}

			double readTimeMs = std::chrono::duration<double, std::milli>(
				std::chrono::steady_clock::now() - sectorStart).count();
			phase2TotalReadTime += readTimeMs;
			phase2TotalProcessed++;

			bool requireCleanC2 = state.hadC2Errors && effectiveConfig.useC2;
			if (requireCleanC2 && sweep >= maxSweeps / 2) {
				requireCleanC2 = false;
			}
			bool readAcceptable = ok && (!requireCleanC2 || c2Errors == 0);

			bool verified = false;
			if (readAcceptable) {
				uint32_t sweepHash = HashSector(buf.data(), AUDIO_SECTOR_SIZE);

				if (sweepHash == state.hash && state.hash != 0) {
					state.matchCount++;
				}
				else {
					state.hash = sweepHash;
					state.matchCount = 1;
					memcpy(disc.rawSectors[state.index].data(), buf.data(), state.sectorSize);
				}

				if (c2Errors == 0) state.hadC2Errors = false;

				int totalPasses = sweep + 2;
				if (state.matchCount >= effectiveConfig.requiredMatches &&
					totalPasses >= effectiveConfig.minPasses) {
					result.secureSectors++;
					result.multiPassSectors++;
					totalPhase2Verified++;
					verified = true;
					if (totalPasses > result.maxPassesRequired)
						result.maxPassesRequired = totalPasses;
				}
				else {
					stillUnverified.push_back(lba);
				}
			}
			else {
				if (c2Errors > 0) log.totalC2Errors += c2Errors;
				stillUnverified.push_back(lba);
			}

			if (logToFile) {
				log.entries.push_back({ lba, state.track, 2, sweep + 2,
					state.matchCount, c2Errors, readTimeMs, verified, state.hash });
			}

			sweepProgress.Update(++sweepCur, sweepTotal);
		}

		sweepProgress.Finish(true, sweepTotal);
		rereadLBAs = std::move(stillUnverified);
	}

	SecureRipPhaseStats phase2Stats;
	phase2Stats.phase = 2;
	phase2Stats.sectorsProcessed = phase2TotalProcessed;
	phase2Stats.sectorsVerified = totalPhase2Verified;
	phase2Stats.sectorsFailed = static_cast<int>(rereadLBAs.size());
	phase2Stats.durationSeconds = std::chrono::duration<double>(
		std::chrono::steady_clock::now() - phase2Start).count();
	phase2Stats.avgReadTimeMs = phase2TotalProcessed > 0
		? phase2TotalReadTime / phase2TotalProcessed : 0.0;
	log.phaseStats.push_back(phase2Stats);

	// ========================================================================
	// PHASE 3: Per-sector rescue for truly stubborn sectors
	// ========================================================================
	if (!rereadLBAs.empty()) {
		std::cout << "\n  Phase 3: " << rereadLBAs.size()
			<< " stubborn sectors — per-sector verification\n";

		ProgressIndicator phase3Progress;
		phase3Progress.SetLabel("  Rescue");
		phase3Progress.Start();

		auto phase3Start = std::chrono::steady_clock::now();
		SecureRipPhaseStats phase3Stats{ 3, 0, 0, 0, 0.0, 0.0 };
		double phase3TotalReadTime = 0.0;

		int phase3Total = static_cast<int>(rereadLBAs.size());
		int phase3Cur = 0;

		for (DWORD lba : rereadLBAs) {
			if (g_interrupt.IsInterrupted() || g_interrupt.CheckEscapeKey()) {
				phase3Progress.Finish(false, phase3Total);
				return false;
			}

			auto& state = sectorStates[lba];

			auto sectorStart = std::chrono::steady_clock::now();

			SecureSectorResult secResult;
			bool ok = ReadSectorSecure(lba, disc.rawSectors[state.index].data(),
				state.sectorSize, state.isAudio, effectiveConfig, secResult);

			double readTimeMs = std::chrono::duration<double, std::milli>(
				std::chrono::steady_clock::now() - sectorStart).count();
			phase3TotalReadTime += readTimeMs;
			phase3Stats.sectorsProcessed++;

			if (secResult.isSecure) {
				result.secureSectors++;
				result.multiPassSectors++;
				phase3Stats.sectorsVerified++;
			}
			else {
				result.unsecureSectors++;
				result.problemSectors.push_back(secResult);
				phase3Stats.sectorsFailed++;
			}

			if (!ok) {
				disc.errorCount++;
				disc.badSectors.push_back(lba);
			}

			if (secResult.passesRequired > result.maxPassesRequired)
				result.maxPassesRequired = secResult.passesRequired;

			if (logToFile) {
				log.entries.push_back({ lba, state.track, 3, secResult.totalPasses,
					secResult.matchingPasses, secResult.c2ErrorPasses,
					readTimeMs, secResult.isSecure, secResult.finalHash });
			}

			phase3Progress.Update(++phase3Cur, phase3Total);
		}

		phase3Stats.durationSeconds = std::chrono::duration<double>(
			std::chrono::steady_clock::now() - phase3Start).count();
		phase3Stats.avgReadTimeMs = phase3Stats.sectorsProcessed > 0
			? phase3TotalReadTime / phase3Stats.sectorsProcessed : 0.0;
		log.phaseStats.push_back(phase3Stats);

		phase3Progress.Finish(true, phase3Total);
	}

	log.totalVerified = result.secureSectors;
	log.totalUnsecure = result.unsecureSectors;
	log.totalDurationSeconds = std::chrono::duration<double>(
		std::chrono::steady_clock::now() - overallStart).count();

	result.securityConfidence = total > 0 ? static_cast<double>(result.secureSectors) / total * 100.0 : 0;
	result.qualityAssessment = result.securityConfidence >= 99.9 ? "Excellent" :
		result.securityConfidence >= 99.0 ? "Good" :
		result.securityConfidence >= 95.0 ? "Acceptable" : "Poor";

	std::cout << "\n  Secure: " << result.secureSectors << "/" << result.totalSectors
		<< " (" << std::fixed << std::setprecision(1) << result.securityConfidence << "%)\n";

	return true;
}