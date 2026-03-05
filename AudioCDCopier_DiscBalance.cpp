#define NOMINMAX
#include "AudioCDCopier.h"
#include "InterruptHandler.h"
#include <iostream>
#include <iomanip>
#include <vector>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>

// ============================================================================
// Disc Balance Check - Detects vibration / wobble by sweeping read speed
// ============================================================================

bool AudioCDCopier::CheckDiscBalance(DiscInfo& disc, int& balanceScore) {
	if (!m_drive.CheckC2Support()) {
		std::cout << "ERROR: C2 support required for disc balance check.\n";
		return false;
	}

	// Probe LiteOn scan availability early — the probe prints diagnostic
	// output, so do it before the progress bar starts.
	bool hasHwC1 = m_drive.SupportsLiteOnScan();

	const int speeds[] = { 4, 8, 16, 24, 32, 40 };
	const int NUM_SPEEDS = sizeof(speeds) / sizeof(speeds[0]);
	const int SAMPLE_COUNT = 50;
	constexpr int READS_PER_SAMPLE = 3;

	// Distance to stay back from target LBA when pre-positioning the head.
	// Must exceed the drive's read-ahead buffer (typically 64-256 KB = 27-109
	// sectors) so the target sector is NOT prefetched into cache.
	constexpr DWORD READ_AHEAD_MARGIN = 150;

	DWORD totalSectors = CalculateTotalAudioSectors(disc);
	if (totalSectors == 0) return false;

	// Build a flat list of all sample LBAs spaced evenly across the disc
	std::vector<DWORD> sampleLBAs;
	DWORD maxLBA = 0;
	for (const auto& t : disc.tracks) {
		if (t.isAudio && t.endLBA > maxLBA) maxLBA = t.endLBA;
	}
	for (int i = 0; i < SAMPLE_COUNT && maxLBA > 0; i++) {
		DWORD lba = static_cast<DWORD>((uint64_t)i * maxLBA / SAMPLE_COUNT);
		// Verify lba falls inside an audio track
		for (const auto& t : disc.tracks) {
			if (!t.isAudio) continue;
			DWORD start = (t.trackNumber == 1) ? 0 : t.pregapLBA;
			if (lba >= start && lba <= t.endLBA) {
				sampleLBAs.push_back(lba);
				break;
			}
		}
	}
	if (sampleLBAs.empty()) return false;

	// Actual sample count may be less than SAMPLE_COUNT on mixed-mode discs
	int totalTests = NUM_SPEEDS * static_cast<int>(sampleLBAs.size()) * READS_PER_SAMPLE;
	int completed = 0;

	std::cout << "\nSweeping " << sampleLBAs.size() << " sample sectors across "
		<< NUM_SPEEDS << " speeds (" << READS_PER_SAMPLE << " reads each)...\n";
	std::cout << "  (Press ESC or Ctrl+C to cancel)\n\n";

	ProgressIndicator progress(40);
	progress.SetLabel("  Balance");
	progress.Start();

	std::vector<BYTE> buf(AUDIO_SECTOR_SIZE);
	std::vector<double> avgC2PerSpeed(NUM_SPEEDS, 0.0);
	std::vector<double> jitterCoeffVar(NUM_SPEEDS, 0.0);
	std::vector<double> avgReadTimeMs(NUM_SPEEDS, 0.0);
	std::vector<double> avgStabilityRatio(NUM_SPEEDS, 1.0);

	for (int s = 0; s < NUM_SPEEDS; s++) {
		m_drive.SetSpeed(speeds[s]);
		Sleep(200); // Let the drive stabilize at new speed

		int totalC2 = 0, tested = 0;
		std::vector<double> readTimesMs;
		readTimesMs.reserve(sampleLBAs.size());
		double stabilitySum = 0.0;
		int stabilityCount = 0;

		for (DWORD lba : sampleLBAs) {
			if (g_interrupt.IsInterrupted() || g_interrupt.CheckEscapeKey()) {
				std::cout << "\n\n*** Balance check cancelled by user ***\n";
				m_drive.SetSpeed(0);
				m_drive.SpinDown();
				progress.Finish(false);
				return false;
			}

			// Take the minimum-time successful read across READS_PER_SAMPLE
			// attempts to strip rotational latency noise, leaving drive
			// behavior as the dominant signal.
			double bestMs = (std::numeric_limits<double>::max)();
			double worstMs = 0.0;
			int bestC2 = 0;
			bool anyOk = false;
			int okCount = 0;

			for (int r = 0; r < READS_PER_SAMPLE; r++) {
				int c2tmp = 0;
				DefeatDriveCache(lba, maxLBA);
				DWORD positionLBA = (lba > READ_AHEAD_MARGIN)
					? lba - READ_AHEAD_MARGIN
					: lba + READ_AHEAD_MARGIN;
				m_drive.ReadSectorAudioOnly(positionLBA, buf.data());

				auto t0 = std::chrono::high_resolution_clock::now();
				bool ok = m_drive.ReadSectorWithC2(lba, buf.data(), nullptr, c2tmp);
				auto t1 = std::chrono::high_resolution_clock::now();
				double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();

				if (ok) {
					anyOk = true;
					okCount++;
					if (ms < bestMs) {
						bestMs = ms;
						bestC2 = c2tmp;
					}
					if (ms > worstMs) worstMs = ms;
				}

				completed++;
				progress.Update(completed, totalTests);
			}

			if (anyOk) {
				readTimesMs.push_back(bestMs);
				// Use worst/best ratio as a per-sector wobble indicator
				if (bestMs > 0.001 && worstMs / bestMs > 3.0)
					totalC2 += 50;  // Synthetic penalty for high intra-sector variance
				totalC2 += bestC2;

				// Track per-sector read stability: worst/best ratio.
				// Wobble causes the same sector to read at wildly different
				// times on successive attempts due to servo hunting.
				if (okCount >= 2 && bestMs > 0.001) {
					stabilitySum += worstMs / bestMs;
					stabilityCount++;
				}
			}
			else {
				totalC2 += 100; // Penalize complete read failures
			}
			tested++;
		}
		avgC2PerSpeed[s] = (tested > 0) ? (double)totalC2 / tested : 0.0;

		// Coefficient of variation = stddev / mean (dimensionless, comparable across speeds)
		if (readTimesMs.size() >= 2) {
			double sum = 0.0;
			for (double t : readTimesMs) sum += t;
			double mean = sum / readTimesMs.size();
			double varSum = 0.0;
			for (double t : readTimesMs) {
				double diff = t - mean;
				varSum += diff * diff;
			}
			double stddev = std::sqrt(varSum / (readTimesMs.size() - 1));
			jitterCoeffVar[s] = (mean > 0.001) ? (stddev / mean) : 0.0;
			avgReadTimeMs[s] = mean;
		}

		// Average per-sector worst/best ratio: 1.0 = perfectly stable,
		// >2.0 = the same sector takes 2× longer on a bad read than a good one.
		avgStabilityRatio[s] = (stabilityCount > 0)
			? stabilitySum / stabilityCount : 1.0;
	}

	progress.Finish(true);
	m_drive.SetSpeed(0);

	// ── Hardware C1 sweep (LiteOn/MediaTek drives only) ─────────────────
	// If the drive supports the 0xF3 vendor scan, collect per-speed C1
	// error rates from the hardware ECC decoder.  This bypasses the broken
	// READ CD C2 bitmap and gives real error data.  Each poll returns one
	// 75-sector time slice, so N polls ~ N seconds of measurement.
	std::vector<double> hwC1PerSpeed(NUM_SPEEDS, 0.0);
	std::vector<double> hwC2PerSpeed(NUM_SPEEDS, 0.0);
	bool hwEccFlat = false;  // True if ECC data has no per-speed discriminating power

	if (hasHwC1) {
		constexpr int HW_SAMPLES_PER_SPEED = 15;

		std::cout << "\nRunning hardware C1/C2 sweep (ECC decoder)...\n";

		// Start scan from the outer 25% of the disc where wobble is worst
		DWORD outerStartLBA = maxLBA * 3 / 4;

		for (int s = 0; s < NUM_SPEEDS; s++) {
			if (g_interrupt.IsInterrupted() || g_interrupt.CheckEscapeKey()) break;

			m_drive.SetSpeed(speeds[s]);
			Sleep(300);

			if (!m_drive.LiteOnScanStart(outerStartLBA, maxLBA)) continue;

			int totalC1 = 0, totalC2 = 0, validSamples = 0;
			bool cancelled = false;
			DWORD firstLBA = 0, lastLBA = 0;
			for (int i = 0; i < HW_SAMPLES_PER_SPEED + 3; i++) {
				if (g_interrupt.IsInterrupted() || g_interrupt.CheckEscapeKey()) {
					cancelled = true;
					break;
				}

				int c1 = 0, c2 = 0, cu = 0;
				DWORD lba = 0;
				bool done = false;

				if (!m_drive.LiteOnScanPoll(c1, c2, cu, lba, done)) break;
				if (done) break;

				// Skip first 3 samples — drive reports accumulated startup errors
				if (i < 3) {
					if (i == 0) firstLBA = lba;
					continue;
				}

				totalC1 += c1;
				totalC2 += c2;
				validSamples++;
				lastLBA = lba;
			}

			m_drive.LiteOnScanStop();  // Always called — even on cancel

			// Log actual scan position for diagnostics
			char dbg[128];
			snprintf(dbg, sizeof(dbg), "HW ECC %dx: LBA %lu-%lu, %d samples, C1=%d C2=%d\n",
				speeds[s], (unsigned long)firstLBA, (unsigned long)lastLBA,
				validSamples, totalC1, totalC2);
			OutputDebugStringA(dbg);

			hwC1PerSpeed[s] = validSamples > 0
				? static_cast<double>(totalC1) / validSamples : 0.0;
			hwC2PerSpeed[s] = validSamples > 0
				? static_cast<double>(totalC2) / validSamples : 0.0;

			if (cancelled) break;
		}

		// Detect flat ECC data: if the scan firmware ignores SetSpeed, all
		// speed steps return the same C1/C2 values and the per-speed
		// comparison is meaningless.  Check if all C1 values are below a
		// noise floor AND the spread across speeds is negligible.
		double maxC1 = 0.0, minC1 = 1e9;
		for (int s = 0; s < NUM_SPEEDS; s++) {
			if (hwC1PerSpeed[s] > maxC1) maxC1 = hwC1PerSpeed[s];
			if (hwC1PerSpeed[s] < minC1) minC1 = hwC1PerSpeed[s];
		}
		if (maxC1 < 0.5 || (maxC1 - minC1) < 0.5) {
			hwEccFlat = true;
		}
	}

	m_drive.SetSpeed(0);
	m_drive.SpinDown();

	// ── Scoring: combine error signal, jitter, and scaling ──────────────

	// Baseline detection: skip clamped speeds at the bottom
	int baselineIdx = 0;
	for (int s = 1; s < NUM_SPEEDS; s++) {
		if (avgReadTimeMs[s] > 0.001 && avgReadTimeMs[baselineIdx] > 0.001) {
			double ratio = avgReadTimeMs[baselineIdx] / avgReadTimeMs[s];
			if (ratio < 1.15) {
				baselineIdx = s;
			}
			else {
				break;
			}
		}
	}

	// Audio-relevant speed ceiling for error scoring.
	// Professional CD players run at 1x; quality rippers at 4x-8x.
	// ECC errors that only appear at 24x+ are likely drive/mechanical
	// artifacts, not disc balance problems.  Cap error scoring at 16x
	// (index 2) so high-speed errors are reported but don't tank the score.
	constexpr int MAX_AUDIO_RELEVANT_SPEED = 16;
	int errorCeilingIdx = NUM_SPEEDS - 1;
	for (int s = 0; s < NUM_SPEEDS; s++) {
		if (speeds[s] >= MAX_AUDIO_RELEVANT_SPEED) {
			errorCeilingIdx = s;
			break;
		}
	}

	// Detect speed fallback: if a higher speed step has read times that
	// match or exceed a much lower speed, the drive silently fell back.
	// Mark those steps so they don't confuse error or scaling analysis.
	std::vector<bool> speedFellBack(NUM_SPEEDS, false);
	for (int s = 2; s < NUM_SPEEDS; s++) {
		if (avgReadTimeMs[s] > 0.001 && avgReadTimeMs[s - 2] > 0.001) {
			// If read time at speed[s] is >= speed[s-2], it fell back
			if (avgReadTimeMs[s] >= avgReadTimeMs[s - 2] * 0.95 &&
				avgReadTimeMs[s - 1] < avgReadTimeMs[s - 2] * 0.85) {
				speedFellBack[s] = true;
			}
		}
	}

	// ECC-specific fallback: if HW errors spike at speed[s-1] but drop to
	// near-zero at speed[s], the drive likely couldn't sustain that speed
	// during the ECC scan and silently fell back to a lower speed.
	// This complements timing-based detection — the two sweeps use
	// different disc regions and the drive may behave differently.
	std::vector<bool> eccFellBack(NUM_SPEEDS, false);
	if (hasHwC1 && !hwEccFlat) {
		for (int s = 1; s < NUM_SPEEDS; s++) {
			double prevErrors = hwC1PerSpeed[s - 1] + hwC2PerSpeed[s - 1];
			double curErrors  = hwC1PerSpeed[s]     + hwC2PerSpeed[s];
			// Previous speed had significant errors but this speed dropped
			// to near-zero — strong sign the drive fell back for the scan
			if (prevErrors > 20.0 && curErrors < 1.0) {
				eccFellBack[s] = true;
			}
		}
	}

	// Error score: compare low-speed baseline to high-speed errors.
	// Prefer hardware C1 data (ECC decoder) over READ CD C2 bitmap —
	// the bitmap is non-functional on many MediaTek-based drives.
	// BUT if the ECC data is flat (firmware ignores SetSpeed), fall through
	// to timing-based scoring — the per-speed comparison is meaningless.
	bool usingHwEcc = false;
	int errorScore;

	if (hasHwC1 && !hwEccFlat) {
		// Hardware C2 score: direct uncorrectable error rate from ECC decoder.
		// Any non-zero C2 at high speed that wasn't present at low speed is a
		// strong wobble signal — the drive's error correction is failing.
		// Only consider speeds up to errorCeilingIdx for scoring.
		double hwC2Baseline = std::max(hwC2PerSpeed[baselineIdx], 0.1);
		double peakHwC2Ratio = 0.0;
		bool anyHwC2 = false;
		for (int s = baselineIdx + 1; s <= errorCeilingIdx; s++) {
			if (speedFellBack[s]) continue;
			if (hwC2PerSpeed[s] > 0.0) anyHwC2 = true;
			double ratio = hwC2PerSpeed[s] / hwC2Baseline;
			if (ratio > peakHwC2Ratio) peakHwC2Ratio = ratio;
		}

		// If hardware C2 errors appear at higher speeds, use that signal
		// directly — it's the strongest indicator of balance problems.
		if (anyHwC2 && peakHwC2Ratio > 1.0) {
			if (peakHwC2Ratio <= 2.0)       errorScore = 75;
			else if (peakHwC2Ratio <= 5.0)  errorScore = 50;
			else if (peakHwC2Ratio <= 10.0) errorScore = 25;
			else                            errorScore = 0;
		}
		else {
			// No hardware C2 — fall back to C1 ratio analysis
			double hwBaseline = std::max(hwC1PerSpeed[baselineIdx], 1.0);
			double peakHwRatio = 0.0;
			for (int s = baselineIdx + 1; s <= errorCeilingIdx; s++) {
				if (speedFellBack[s]) continue;
				double ratio = hwC1PerSpeed[s] / hwBaseline;
				if (ratio > peakHwRatio) peakHwRatio = ratio;
			}

			if (peakHwRatio <= 2.0)       errorScore = 100;
			else if (peakHwRatio <= 4.0)  errorScore = 75;
			else if (peakHwRatio <= 8.0)  errorScore = 50;
			else if (peakHwRatio <= 16.0) errorScore = 25;
			else                          errorScore = 0;

			// C1 trend detection (only within audio-relevant range)
			int numActive = (errorCeilingIdx + 1) - baselineIdx;
			if (numActive >= 4) {
				int half = numActive / 2;
				double lowSum = 0.0, highSum = 0.0;
				for (int i = 0; i < half; i++)
					lowSum += hwC1PerSpeed[baselineIdx + i];
				for (int i = half; i < numActive; i++)
					highSum += hwC1PerSpeed[baselineIdx + i];

				double lowAvg = lowSum / half;
				double highAvg = highSum / (numActive - half);
				double trendRatio = (lowAvg > 0.1) ? highAvg / lowAvg : 0.0;

				if (trendRatio > 1.8 && errorScore > 75)  errorScore = 75;
				if (trendRatio > 3.0 && errorScore > 50)  errorScore = 50;
				if (trendRatio > 6.0 && errorScore > 25)  errorScore = 25;
			}
		}

		usingHwEcc = true;
	}
	else {
		double baseline = std::max(avgC2PerSpeed[baselineIdx], 1.0);
		double peakC2Ratio = 0.0;
		for (int s = baselineIdx + 1; s <= errorCeilingIdx; s++) {
			if (speedFellBack[s]) continue;
			double ratio = avgC2PerSpeed[s] / baseline;
			if (ratio > peakC2Ratio) peakC2Ratio = ratio;
		}

		if (peakC2Ratio <= 1.5)       errorScore = 100;
		else if (peakC2Ratio <= 3.0)  errorScore = 75;
		else if (peakC2Ratio <= 8.0)  errorScore = 50;
		else if (peakC2Ratio <= 20.0) errorScore = 25;
		else                          errorScore = 0;
	}

	// Jitter score
	double baselineCV = std::max(jitterCoeffVar[baselineIdx], 0.01);
	double peakJitterRatio = 0.0;
	for (int s = baselineIdx + 1; s < NUM_SPEEDS; s++) {
		double ratio = jitterCoeffVar[s] / baselineCV;
		if (ratio > peakJitterRatio) peakJitterRatio = ratio;
	}

	int jitterScore;
	if (peakJitterRatio <= 2.0)       jitterScore = 100;
	else if (peakJitterRatio <= 4.0)  jitterScore = 75;
	else if (peakJitterRatio <= 8.0)  jitterScore = 50;
	else if (peakJitterRatio <= 16.0) jitterScore = 25;
	else                              jitterScore = 0;

	// Read stability score: per-sector worst/best ratio increase with speed.
	// This directly measures wobble — a balanced disc reads the same sector
	// in the same time regardless of attempt, while wobble causes the servo
	// to hunt, producing large read-time spread for individual sectors.
	double baselineStability = std::max(avgStabilityRatio[baselineIdx], 1.001);
	double peakStabilityRatio = 0.0;
	for (int s = baselineIdx + 1; s < NUM_SPEEDS; s++) {
		double ratio = avgStabilityRatio[s] / baselineStability;
		if (ratio > peakStabilityRatio) peakStabilityRatio = ratio;
	}

	int stabilityScore;
	if (peakStabilityRatio <= 1.5)       stabilityScore = 100;
	else if (peakStabilityRatio <= 2.5)  stabilityScore = 75;
	else if (peakStabilityRatio <= 4.0)  stabilityScore = 50;
	else if (peakStabilityRatio <= 8.0)  stabilityScore = 25;
	else                                 stabilityScore = 0;

	// Also check absolute stability at top speed — even without a baseline
	// increase, a high worst/best ratio at max speed indicates wobble.
	double maxStability = *std::max_element(avgStabilityRatio.begin(),
		avgStabilityRatio.end());
	if (maxStability > 3.0 && stabilityScore > 75)  stabilityScore = 75;
	if (maxStability > 5.0 && stabilityScore > 50)  stabilityScore = 50;
	if (maxStability > 10.0 && stabilityScore > 25) stabilityScore = 25;

	// Detect drive speed ceiling — mirror of the baseline detection but from
	// the top.  If the drive caps its speed, adjacent high-speed steps will
	// have nearly identical read times.  Only collapse steps where the higher
	// requested speed is genuinely faster (or equal) — a regression where the
	// "faster" setting produces equal or slower times is a wobble signal, NOT
	// a speed cap.  Use strict less-than to avoid collapsing ties.
	int ceilingIdx = NUM_SPEEDS - 1;
	for (int s = NUM_SPEEDS - 2; s > baselineIdx; s--) {
		if (avgReadTimeMs[s] > 0.001 && avgReadTimeMs[ceilingIdx] > 0.001) {
			double ratio = avgReadTimeMs[s] / avgReadTimeMs[ceilingIdx];
			if (ratio < 1.15 && avgReadTimeMs[ceilingIdx] < avgReadTimeMs[s]) {
				ceilingIdx = s;
			}
			else {
				break;
			}
		}
	}

	// Speed scaling score: detect when the drive fails to go faster at higher
	// speed settings.  Wobble causes the servo to struggle, so the drive
	// plateaus or even regresses — read times stop decreasing or increase.
	// Compare adjacent speed pairs between baseline and ceiling only.
	int scalingScore = 100;
	int scalingPenalties = 0;
	for (int s = baselineIdx + 1; s <= ceilingIdx; s++) {
		int prev = (s == baselineIdx + 1) ? baselineIdx : s - 1;
		if (avgReadTimeMs[s] < 0.001 || avgReadTimeMs[prev] < 0.001) continue;

		double timeRatio = avgReadTimeMs[s] / avgReadTimeMs[prev];

		if (timeRatio >= 1.0) {
			// Regression: higher speed is actually slower (or equal)
			scalingPenalties += 2;
		}
		else if (timeRatio > 0.95) {
			// Plateau: < 5% improvement despite a speed step increase
			scalingPenalties += 1;
		}
	}

	// After collecting avgReadTimeMs[], detect if the drive throttled.
	// If the requested speed doubled but read time barely changed, the drive
	// refused to go faster — a strong wobble indicator.
	int throttlePenalties = 0;
	for (int s = baselineIdx + 1; s <= ceilingIdx; s++) {
		if (avgReadTimeMs[s] < 0.001 || avgReadTimeMs[baselineIdx] < 0.001)
			continue;

		// Expected time ratio if speed scaling were perfect
		double expectedRatio = static_cast<double>(speeds[baselineIdx])
			/ speeds[s];
		// Actual time ratio
		double actualRatio = avgReadTimeMs[s] / avgReadTimeMs[baselineIdx];

		// If actual is more than 1.5× the expected, drive is throttling
		if (actualRatio > expectedRatio * 1.5)
			throttlePenalties++;
	}
	if (throttlePenalties >= 2 && scalingScore > 75) scalingScore = 75;
	if (throttlePenalties >= 3 && scalingScore > 50) scalingScore = 50;

	if (scalingPenalties == 0)      scalingScore = std::min(scalingScore, 100);
	else if (scalingPenalties == 1) scalingScore = std::min(scalingScore, 75);
	else if (scalingPenalties == 2) scalingScore = std::min(scalingScore, 50);
	else if (scalingPenalties <= 4) scalingScore = std::min(scalingScore, 25);
	else                            scalingScore = 0;

	// Final score: take the worst of all signals
	balanceScore = std::min({ errorScore, jitterScore, scalingScore, stabilityScore });

	// ── Report ──────────────────────────────────────────────────────────
	std::cout << "\n" << std::string(60, '=') << "\n";
	std::cout << "              DISC BALANCE CHECK\n";
	std::cout << std::string(60, '=') << "\n";
	std::cout << "  (Detects vibration / wobble by sweeping read speed)\n\n";

	if (hasHwC1) {
		std::cout << "--- Hardware Error Rates by Speed (ECC decoder) ---\n";
		for (int s = 0; s < NUM_SPEEDS; s++) {
			std::cout << "  " << std::setw(3) << speeds[s] << "x:  C1 "
				<< std::fixed << std::setprecision(1) << hwC1PerSpeed[s]
				<< "/sec   C2 " << std::setprecision(1) << hwC2PerSpeed[s]
				<< "/sec";
			if (speedFellBack[s] || eccFellBack[s])
				std::cout << "  ** FALLBACK (drive can't sustain this speed) **";
			else if (speeds[s] > MAX_AUDIO_RELEVANT_SPEED)
				std::cout << "  (informational — above audio-relevant range)";
			std::cout << "\n";
		}
		if (hwEccFlat) {
			std::cout << "  ** NOTE: C1/C2 rates are flat across all speed settings.\n"
				<< "     The 0xF3 scan firmware likely ignores SetSpeed — per-speed\n"
				<< "     comparison is not meaningful. Relying on timing-based\n"
				<< "     scoring (Jitter/Scaling) instead of ECC for wobble. **\n";
		}
		// Note which speeds contributed to scoring
		bool anyAboveCeiling = false;
		for (int s = errorCeilingIdx + 1; s < NUM_SPEEDS; s++) {
			if (hwC1PerSpeed[s] > 0.0 || hwC2PerSpeed[s] > 0.0)
				anyAboveCeiling = true;
		}
		if (anyAboveCeiling) {
			std::cout << "  Note: Errors above " << MAX_AUDIO_RELEVANT_SPEED
				<< "x are not scored — no audio player operates at those speeds.\n";
		}
	}
	else {
		std::cout << "--- C2 Errors by Speed ---\n";
		for (int s = 0; s < NUM_SPEEDS; s++) {
			std::cout << "  " << std::setw(3) << speeds[s] << "x:  "
				<< std::fixed << std::setprecision(2) << avgC2PerSpeed[s]
				<< " avg C2/sector\n";
		}

		// Warn when C2 reports zero but timing-based metrics found problems.
		// This combination strongly suggests the drive accepts C2 commands
		// without actually populating the error pointer data.
		bool allC2Zero = true;
		for (int s = 0; s < NUM_SPEEDS; s++) {
			if (avgC2PerSpeed[s] > 0.0) { allC2Zero = false; break; }
		}
		if (allC2Zero && scalingScore < 100) {
			std::cout << "  ** NOTE: C2 reports 0 errors at all speeds, but timing\n"
				<< "     detected wobble. C2 data may not be functional on this\n"
				<< "     drive. Rely on Scaling/Jitter scores instead. **\n";
		}
	}

	std::cout << "\n--- Read Time Jitter by Speed ---\n";
	for (int s = 0; s < NUM_SPEEDS; s++) {
		std::cout << "  " << std::setw(3) << speeds[s] << "x:  CV "
			<< std::fixed << std::setprecision(3) << jitterCoeffVar[s]
			<< "  (avg " << std::setprecision(1) << avgReadTimeMs[s] << " ms)"
			<< "  stability " << std::setprecision(2) << avgStabilityRatio[s] << "x";
		// Flag speed regression/plateau inline
		if (s > baselineIdx && s <= ceilingIdx) {
			int prev = (s == baselineIdx + 1) ? baselineIdx : s - 1;
			if (avgReadTimeMs[prev] > 0.001 && avgReadTimeMs[s] >= avgReadTimeMs[prev])
				std::cout << "  ** REGRESSION **";
			else if (avgReadTimeMs[prev] > 0.001 && avgReadTimeMs[s] > avgReadTimeMs[prev] * 0.95)
				std::cout << "  * plateau *";
		}
		std::cout << "\n";
	}

	// Detect if the drive ignored the lowest speed setting
	if (NUM_SPEEDS >= 2 && avgReadTimeMs[0] > 0.001 && avgReadTimeMs[1] > 0.001) {
		double speedRatio = avgReadTimeMs[0] / avgReadTimeMs[1];
		if (speedRatio < 1.15) {
			std::cout << "  Note: Drive may not support " << speeds[0]
				<< "x (times match " << speeds[1] << "x). Baseline uses actual drive minimum.\n";
		}
	}

	std::cout << "\n  Error Sub-Score:     " << errorScore << " / 100";
	if (usingHwEcc) std::cout << "  (hardware ECC)";
	else if (hasHwC1 && hwEccFlat) std::cout << "  (C2 bitmap — ECC flat, ignored)";
	else            std::cout << "  (C2 bitmap)";
	std::cout << "\n";
	std::cout << "  Jitter Sub-Score:    " << jitterScore << " / 100\n";
	std::cout << "  Stability Sub-Score: " << stabilityScore << " / 100  (per-sector read consistency)\n";
	std::cout << "  Scaling Sub-Score:   " << scalingScore << " / 100\n";

	std::cout << "\n  Balance Score: " << balanceScore << " / 100";
	if (balanceScore >= 75)      std::cout << "  (GOOD - disc is well balanced)\n";
	else if (balanceScore >= 50) std::cout << "  (FAIR - some wobble detected, reduce rip speed)\n";
	else                         std::cout << "  (POOR - significant balance problem, use 4x-8x max)\n";

	std::cout << std::string(60, '=') << "\n";
	return true;
}