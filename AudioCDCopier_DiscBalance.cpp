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

// Continuous score from a ratio using log-linear interpolation.
// Breakpoints are {ratio, score} pairs in ascending ratio order.
// Interpolates linearly in log-space between adjacent breakpoints.
static int ContinuousScore(double ratio,
	std::initializer_list<std::pair<double, int>> bpList) {
	std::vector<std::pair<double, int>> bp(bpList);
	if (ratio <= bp.front().first) return bp.front().second;
	if (ratio >= bp.back().first) return bp.back().second;

	double logR = std::log(std::max(ratio, 1e-9));
	for (size_t i = 1; i < bp.size(); i++) {
		if (ratio <= bp[i].first) {
			double logLo = std::log(std::max(bp[i - 1].first, 1e-9));
			double logHi = std::log(std::max(bp[i].first, 1e-9));
			double t = (logHi > logLo)
				? (logR - logLo) / (logHi - logLo) : 0.0;
			double score = bp[i - 1].second
				+ t * (bp[i].second - bp[i - 1].second);
			return std::clamp(static_cast<int>(score + 0.5), 0, 100);
		}
	}
	return bp.back().second;
}

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
	// Build sample LBAs with outer-edge bias: use a quadratic distribution
	// so ~60% of samples fall in the outer 40% of the disc, where wobble
	// effects are strongest (centrifugal force ∝ radius²).
	for (int i = 0; i < SAMPLE_COUNT && maxLBA > 0; i++) {
		double t = static_cast<double>(i) / SAMPLE_COUNT;
		// Concave bias toward outer edge: 1-(1-t)² = 2t - t²
		// maps [0,1] → [0,1] with higher sample density near 1.0
		double biased = 2.0 * t - t * t;
		// Blend 50% uniform + 50% concave to keep some inner coverage
		double blended = 0.5 * t + 0.5 * biased;
		DWORD lba = static_cast<DWORD>(blended * maxLBA);

		for (const auto& tr : disc.tracks) {
			if (!tr.isAudio) continue;
			DWORD start = (tr.trackNumber == 1) ? 0 : tr.pregapLBA;
			if (lba >= start && lba <= tr.endLBA) {
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
		// Trimmed mean + CV: drop top/bottom 10% to resist OS/USB outliers
		if (readTimesMs.size() >= 5) {
			std::vector<double> sorted = readTimesMs;
			std::sort(sorted.begin(), sorted.end());
			size_t trimCount = sorted.size() / 10;  // 10% from each tail
			if (trimCount == 0) trimCount = 1;       // always trim at least 1

			double trimSum = 0.0;
			size_t trimN = 0;
			for (size_t i = trimCount; i < sorted.size() - trimCount; i++) {
				trimSum += sorted[i];
				trimN++;
			}

			double trimMean = trimSum / trimN;
			double trimVarSum = 0.0;
			for (size_t i = trimCount; i < sorted.size() - trimCount; i++) {
				double diff = sorted[i] - trimMean;
				trimVarSum += diff * diff;
			}
			double trimStddev = std::sqrt(trimVarSum / (trimN - 1));
			jitterCoeffVar[s] = (trimMean > 0.001) ? (trimStddev / trimMean) : 0.0;
			avgReadTimeMs[s] = trimMean;
		}
		else if (readTimesMs.size() >= 2) {
			// Too few samples to trim — fall back to raw stats
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

		// CAV detrend: at high speeds, remove the inner-to-outer gradient
		// so that normal CAV positional variance doesn't inflate jitter.
		// readTimesMs[] is already in ascending LBA order.
		if (speeds[s] >= 16 && readTimesMs.size() >= 10) {
			// Build paired (LBA, time) and sort by time to trim outliers
			struct Sample { double lba; double ms; };
			std::vector<Sample> samples(readTimesMs.size());
			for (size_t i = 0; i < readTimesMs.size(); i++) {
				samples[i] = { static_cast<double>(sampleLBAs[i]), readTimesMs[i] };
			}
			std::sort(samples.begin(), samples.end(),
				[](const Sample& a, const Sample& b) { return a.ms < b.ms; });

			size_t trimN = samples.size() / 10;
			if (trimN == 0) trimN = 1;
			size_t lo = trimN, hi = samples.size() - trimN;

			// Linear regression on trimmed data, using LBA as X
			double sumX = 0, sumY = 0, sumXY = 0, sumX2 = 0;
			size_t n = hi - lo;
			for (size_t i = lo; i < hi; i++) {
				sumX += samples[i].lba;  sumY += samples[i].ms;
				sumXY += samples[i].lba * samples[i].ms;
				sumX2 += samples[i].lba * samples[i].lba;
			}
			double denom = n * sumX2 - sumX * sumX;
			if (n >= 5 && std::abs(denom) > 1e-12) {
				double slope = (n * sumXY - sumX * sumY) / denom;
				double intercept = (sumY - slope * sumX) / n;

				// Only detrend if slope is negative (outer = faster, i.e. CAV)
				if (slope < 0.0) {
					double mean = sumY / n;
					double residSqSum = 0.0;
					for (size_t i = lo; i < hi; i++) {
						double residual = samples[i].ms
							- (intercept + slope * samples[i].lba);
						residSqSum += residual * residual;
					}
					double residStddev = std::sqrt(residSqSum / (n - 1));
					double detrendedCV = (mean > 0.001)
						? (residStddev / mean) : 0.0;
					jitterCoeffVar[s] = std::min(jitterCoeffVar[s], detrendedCV);
				}
			}
		}

		// Average per-sector worst/best ratio: 1.0 = perfectly stable,
		// >2.0 = the same sector takes 2× longer on a bad read than a good one.
		avgStabilityRatio[s] = (stabilityCount > 0)
			? stabilitySum / stabilityCount : 1.0;
	}

	progress.Finish(true);

	// ── Thermal drift check ────────────────────────────────────────────
	// Re-test baseline speed to detect if disc heating shifted read times.
	double driftRatio = 1.0;
	{
		m_drive.SetSpeed(speeds[0]);
		Sleep(300);

		constexpr int DRIFT_SAMPLES = 10;
		int driftCount = std::min(DRIFT_SAMPLES, static_cast<int>(sampleLBAs.size()));
		double driftSum = 0.0;
		int driftValid = 0;

		// Use evenly-spaced samples across the full disc (matching the
		// original sweep's mix of inner/outer) for an apples-to-apples
		// comparison with avgReadTimeMs[0].
		int step = std::max(1, static_cast<int>(sampleLBAs.size()) / driftCount);
		for (int i = 0; i < driftCount; i++) {
			if (g_interrupt.IsInterrupted() || g_interrupt.CheckEscapeKey()) break;

			int idx = std::min(i * step,
				static_cast<int>(sampleLBAs.size()) - 1);
			DWORD lba = sampleLBAs[idx];
			DefeatDriveCache(lba, maxLBA);
			DWORD positionLBA = (lba > READ_AHEAD_MARGIN)
				? lba - READ_AHEAD_MARGIN : lba + READ_AHEAD_MARGIN;
			m_drive.ReadSectorAudioOnly(positionLBA, buf.data());

			int c2tmp = 0;
			auto t0 = std::chrono::high_resolution_clock::now();
			bool ok = m_drive.ReadSectorWithC2(lba, buf.data(), nullptr, c2tmp);
			auto t1 = std::chrono::high_resolution_clock::now();
			double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();

			if (ok) { driftSum += ms; driftValid++; }
		}

		if (driftValid > 0 && avgReadTimeMs[0] > 0.001) {
			double retest = driftSum / driftValid;
			driftRatio = retest / avgReadTimeMs[0];
		}
	}

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
			double curErrors = hwC1PerSpeed[s] + hwC2PerSpeed[s];
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
			errorScore = ContinuousScore(peakHwC2Ratio, {
				{1.0, 90}, {2.0, 75}, {5.0, 50}, {10.0, 25}, {20.0, 0}
				});
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

			errorScore = ContinuousScore(peakHwRatio, {
				{1.0, 100}, {2.0, 100}, {4.0, 75}, {8.0, 50}, {16.0, 25}, {32.0, 0}
				});

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

				int trendCap = ContinuousScore(trendRatio, {
					{1.0, 100}, {1.8, 75}, {3.0, 50}, {6.0, 25}, {12.0, 0}
					});
				errorScore = std::min(errorScore, trendCap);
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

		errorScore = ContinuousScore(peakC2Ratio, {
			{1.0, 100}, {1.5, 100}, {3.0, 75}, {8.0, 50}, {20.0, 25}, {40.0, 0}
			});
	}

	// Jitter score
	double baselineCV = std::max(jitterCoeffVar[baselineIdx], 0.01);
	double peakJitterRatio = 0.0;
	for (int s = baselineIdx + 1; s < NUM_SPEEDS; s++) {
		double ratio = jitterCoeffVar[s] / baselineCV;
		if (ratio > peakJitterRatio) peakJitterRatio = ratio;
	}

	int jitterScore = ContinuousScore(peakJitterRatio, {
		{1.0, 100}, {2.0, 100}, {4.0, 75}, {8.0, 50}, {16.0, 25}, {32.0, 0}
		});

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

	int stabilityScore = ContinuousScore(peakStabilityRatio, {
		{1.0, 100}, {1.5, 100}, {2.5, 75}, {4.0, 50}, {8.0, 25}, {16.0, 0}
		});

	// Also check absolute stability at top speed — cap score based on
	// raw worst/best ratio regardless of baseline comparison.
	double maxStability = *std::max_element(avgStabilityRatio.begin(),
		avgStabilityRatio.end());
	int absoluteStabilityCap = ContinuousScore(maxStability, {
		{1.0, 100}, {3.0, 75}, {5.0, 50}, {10.0, 25}, {20.0, 0}
		});
	stabilityScore = std::min(stabilityScore, absoluteStabilityCap);

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

	// Estimate the drive's actual speed at the baseline index.
	// If the drive ignored lower requested speeds and ran at its own minimum,
	// use the first differentiated speed step to back-calculate from read
	// times (time is inversely proportional to speed).
	double actualBaselineSpeed = static_cast<double>(speeds[baselineIdx]);
	if (baselineIdx + 1 < NUM_SPEEDS
		&& avgReadTimeMs[baselineIdx] > 0.001
		&& avgReadTimeMs[baselineIdx + 1] > 0.001) {
		double estimated = speeds[baselineIdx + 1]
			* avgReadTimeMs[baselineIdx + 1] / avgReadTimeMs[baselineIdx];
		// Clamp: actual speed is between requested and next step
		actualBaselineSpeed = std::clamp(estimated,
			static_cast<double>(speeds[baselineIdx]),
			static_cast<double>(speeds[baselineIdx + 1]));
	}

	int throttlePenalties = 0;
	for (int s = baselineIdx + 1; s <= ceilingIdx; s++) {
		if (avgReadTimeMs[s] < 0.001 || avgReadTimeMs[baselineIdx] < 0.001)
			continue;

		// Expected time ratio if speed scaling were perfect
		double expectedRatio = actualBaselineSpeed / speeds[s];
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

	// Final score: concordance-adjusted combination.
	// Start with the worst sub-score (conservative baseline), then apply
	// a penalty when multiple signals independently confirm degradation.
	// This separates "one noisy metric" from "real multi-signal wobble."
	int subScores[] = { errorScore, jitterScore, scalingScore, stabilityScore };
	std::sort(std::begin(subScores), std::end(subScores));

	int worst = subScores[0];
	int secondWorst = subScores[1];

	// Concordance penalty: each additional degraded signal (<75) adds
	// confidence that wobble is real, not measurement noise.
	int degradedCount = 0;
	for (int sc : subScores) {
		if (sc < 75) degradedCount++;
	}
	int concordancePenalty = (degradedCount >= 2) ? (degradedCount - 1) * 5 : 0;

	// Blend worst and second-worst to soften single-metric false positives.
	// If only one metric is bad, the blend pulls toward the healthy second.
	// If both are bad, the blend stays low.
	int blended = (worst * 7 + secondWorst * 3) / 10;

	// The blend can be at most as generous as the second-worst score,
	// but never more generous than 'worst + 10' to prevent over-softening.
	blended = std::min(blended, worst + 10);

	balanceScore = std::max(0, blended - concordancePenalty);

	// Determine the highest speed that showed no wobble degradation.
	// Walk up from baseline; stop at the first speed with a regression,
	// plateau, fallback, or significant error/stability increase.
	int safeSpeedIdx = baselineIdx;
	for (int s = baselineIdx + 1; s < NUM_SPEEDS; s++) {
		if (speedFellBack[s] || eccFellBack[s]) break;

		// Check for timing regression or plateau
		int prev = (s == baselineIdx + 1) ? baselineIdx : s - 1;
		if (avgReadTimeMs[s] > 0.001 && avgReadTimeMs[prev] > 0.001) {
			double timeRatio = avgReadTimeMs[s] / avgReadTimeMs[prev];
			if (timeRatio >= 0.95) break;  // plateau or regression

			// Also check against expected scaling from baseline
			if (avgReadTimeMs[baselineIdx] > 0.001) {
				double expectedRatio = actualBaselineSpeed / speeds[s];
				double actualRatio = avgReadTimeMs[s] / avgReadTimeMs[baselineIdx];
				if (actualRatio > expectedRatio * 1.5) break;  // throttled
			}
		}

		// Check for stability degradation
		if (avgStabilityRatio[s] > 2.0 * avgStabilityRatio[baselineIdx])
			break;

		// Check for ECC error spike (if available)
		if (usingHwEcc) {
			double baseC1 = std::max(hwC1PerSpeed[baselineIdx], 1.0);
			if (hwC1PerSpeed[s] / baseC1 > 3.0) break;
			if (hwC2PerSpeed[s] > 0.5) break;
		}

		safeSpeedIdx = s;
	}
	int safeSpeed = speeds[safeSpeedIdx];

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
			int prev = s - 1;
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

	if (std::isfinite(driftRatio) && (driftRatio > 1.3 || driftRatio < 0.7)) {
		std::cout << "\n  ** WARNING: Baseline re-test shows "
			<< std::fixed << std::setprecision(0) << (std::abs(driftRatio - 1.0) * 100)
			<< "% thermal drift. Scores may be affected by disc heating. **\n";
	}

	std::cout << "\n  Balance Score: " << balanceScore << " / 100";
	if (balanceScore >= 75)      std::cout << "  (GOOD - disc is well balanced)\n";
	else if (balanceScore >= 50) std::cout << "  (FAIR - some wobble detected, reduce rip speed)\n";
	else                         std::cout << "  (POOR - significant balance problem, use 4x-8x max)\n";

	std::cout << "  Max Safe Rip Speed: " << safeSpeed << "x\n";

	if (balanceScore < 75) {
		std::cout << "\n  Recommendation:\n";
		if (balanceScore < 50) {
			std::cout << "    - Re-rip at " << safeSpeed
				<< "x or lower with Secure or Paranoid mode.\n";
			std::cout << "    - Any previous rip above " << safeSpeed
				<< "x should be considered suspect.\n";
			std::cout << "    - Verify against AccurateRip -- if CRCs match,\n"
				<< "      the existing rip is bit-perfect despite wobble.\n";
		}
		else {
			std::cout << "    - Rips at or below " << safeSpeed
				<< "x are reliable.\n";
			std::cout << "    - Rips above " << safeSpeed
				<< "x without Secure mode should be re-verified.\n";
			std::cout << "    - AccurateRip match = no re-copy needed.\n";
		}
	}

	std::cout << std::string(60, '=') << "\n";
	return true;
}