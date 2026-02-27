#define NOMINMAX
#include "AudioCDCopier.h"
#include "InterruptHandler.h"
#include <iostream>
#include <iomanip>
#include <vector>
#include <algorithm>
#include <chrono>
#include <cmath>

// ============================================================================
// Performance Tests - Speed Comparison & Seek Analysis
// ============================================================================

bool AudioCDCopier::RunSpeedComparisonTest(DiscInfo& disc, std::vector<SpeedComparisonResult>& results) {
	std::cout << "\n=== Speed Comparison Test ===\n";
	if (!m_drive.CheckC2Support()) {
		std::cout << "ERROR: C2 support required.\n";
		return false;
	}

	DWORD totalSectors = CalculateTotalAudioSectors(disc);
	if (totalSectors == 0) return false;

	int sampleInterval = std::max(1, static_cast<int>(totalSectors / 100));
	int totalSamples = std::max(1, static_cast<int>(totalSectors / sampleInterval) + 1);
	results.clear();
	int tested = 0;

	std::cout << "Testing ~" << totalSamples << " sample sectors at 4x vs 24x...\n";
	std::cout << "  (Press ESC or Ctrl+C to cancel)\n\n";

	ProgressIndicator progress(40);
	progress.SetLabel("  Speed Test");
	progress.Start();

	std::vector<BYTE> buf(AUDIO_SECTOR_SIZE);

	for (const auto& t : disc.tracks) {
		if (!t.isAudio) continue;
		DWORD start = (t.trackNumber == 1) ? 0 : t.pregapLBA;
		for (DWORD lba = start; lba <= t.endLBA; lba += sampleInterval) {
			if (g_interrupt.IsInterrupted() || g_interrupt.CheckEscapeKey()) {
				std::cout << "\n\n*** Test cancelled by user ***\n";
				m_drive.SetSpeed(0);
				progress.Finish(false);
				return false;
			}

			SpeedComparisonResult r = { lba, 0, 0, false };
			bool lowOk = false, highOk = false;

			DefeatDriveCache(lba, t.endLBA);

			m_drive.SetSpeed(4);
			Sleep(100);
			lowOk = m_drive.ReadSectorWithC2(lba, buf.data(), nullptr, r.lowSpeedC2);

			DefeatDriveCache(lba, t.endLBA);

			m_drive.SetSpeed(24);
			Sleep(100);
			highOk = m_drive.ReadSectorWithC2(lba, buf.data(), nullptr, r.highSpeedC2);

			if (!lowOk) r.lowSpeedC2 = -1;
			if (!highOk) r.highSpeedC2 = -1;

			// Improved inconsistency detection: more sensitive to small error counts
			r.inconsistent =
				(!lowOk || !highOk) ||
				(r.lowSpeedC2 == 0 && r.highSpeedC2 > 0) ||
				(r.highSpeedC2 == 0 && r.lowSpeedC2 > 0) ||
				(r.highSpeedC2 > 0 && r.highSpeedC2 > r.lowSpeedC2 * 2) ||
				(r.lowSpeedC2 > 0 && r.lowSpeedC2 > r.highSpeedC2 * 2);

			if (r.lowSpeedC2 != 0 || r.highSpeedC2 != 0) results.push_back(r);
			tested++;
			progress.Update(tested, totalSamples);
		}
	}
	progress.Finish(true);
	m_drive.SetSpeed(0);
	m_drive.SpinDown();

	int lowSpeedErrors = 0, highSpeedErrors = 0, inconsistentCount = 0;
	int lowFailures = 0, highFailures = 0;
	for (const auto& r : results) {
		if (r.lowSpeedC2 < 0) lowFailures++;
		else lowSpeedErrors += r.lowSpeedC2;
		if (r.highSpeedC2 < 0) highFailures++;
		else highSpeedErrors += r.highSpeedC2;
		if (r.inconsistent) inconsistentCount++;
	}

	std::cout << "\n" << std::string(60, '=') << "\n";
	std::cout << "              SPEED COMPARISON REPORT\n";
	std::cout << std::string(60, '=') << "\n";
	std::cout << "  (Compares read errors at slow vs fast speed to find optimal rip speed)\n\n";

	std::cout << "--- Results ---\n";
	std::cout << "  Sectors tested:       " << tested << "\n";
	std::cout << "  Low speed  (4x)  C2:  " << lowSpeedErrors;
	if (lowFailures > 0) std::cout << " (+" << lowFailures << " read failures)";
	std::cout << "\n";
	std::cout << "  High speed (24x) C2:  " << highSpeedErrors;
	if (highFailures > 0) std::cout << " (+" << highFailures << " read failures)";
	std::cout << "\n";
	std::cout << "  Inconsistent sectors: " << inconsistentCount;
	if (tested > 0)
		std::cout << " (" << std::fixed << std::setprecision(1)
		<< (inconsistentCount * 100.0 / tested) << "%)";
	std::cout << "\n";

	// Improved error ratio reporting: handles asymmetric failures
	if (highSpeedErrors > 0 || lowSpeedErrors > 0) {
		if (highSpeedErrors > 0 && lowSpeedErrors > 0) {
			double ratio = static_cast<double>(highSpeedErrors) / lowSpeedErrors;
			std::cout << "  Error ratio (24x/4x): " << std::fixed << std::setprecision(1) << ratio << "x";
			if (ratio > 3.0) std::cout << "  (high speed significantly worse)";
			else if (ratio < 0.5) std::cout << "  (low speed worse - unusual)";
			else std::cout << "  (similar at both speeds)";
			std::cout << "\n";
		}
		else if (highSpeedErrors > 0 && lowSpeedErrors == 0) {
			std::cout << "  Error ratio (24x/4x): HIGH (errors only at high speed)\n";
		}
		else if (lowSpeedErrors > 0 && highSpeedErrors == 0) {
			std::cout << "  Error ratio (24x/4x): <0.1 (errors only at low speed - unusual)\n";
		}
	}

	std::cout << "\n--- Recommendation ---\n  Optimal rip speed: ";
	if (lowFailures > 0 || highFailures > 0) {
		std::cout << "2-4x\n";
		std::cout << "  Read failures detected. Use the slowest reliable speed.\n";
	}
	else if (results.empty() || (lowSpeedErrors == 0 && highSpeedErrors == 0)) {
		std::cout << "Any (24x safe)\n";
		std::cout << "  Disc reads cleanly at all speeds. No benefit from slowing down.\n";
	}
	else if (highSpeedErrors > lowSpeedErrors * 2 || inconsistentCount > tested / 10) {
		std::cout << "4x\n";
		std::cout << "  Significant error increase at high speed. Slow rip strongly recommended.\n";
	}
	else if (highSpeedErrors > lowSpeedErrors) {
		std::cout << "4x\n";
		std::cout << "  Error increase at high speed. Speeds above 4x not recommended.\n";
	}
	else {
		std::cout << "Any (24x safe)\n";
		std::cout << "  No significant benefit from slower speeds.\n";
	}
	std::cout << std::string(60, '=') << "\n";

	return true;
}

bool AudioCDCopier::RunSeekTimeAnalysis(DiscInfo& disc, std::vector<SeekTimeResult>& results) {
	std::cout << "\n=== Seek Time Analysis ===\n";
	results.clear();

	std::vector<std::pair<DWORD, DWORD>> audioRanges;
	DWORD totalSectors = 0;
	for (const auto& t : disc.tracks) {
		if (!t.isAudio) continue;
		DWORD start = (t.trackNumber == 1) ? 0 : t.pregapLBA;
		audioRanges.push_back({ start, t.endLBA });
		totalSectors += t.endLBA - start + 1;
	}

	if (totalSectors == 0) {
		std::cout << "No audio tracks to analyze.\n";
		return false;
	}

	m_drive.SetSpeed(0);
	std::cout << "Testing seek times across disc surface...\n";
	std::cout << "  (Press ESC or Ctrl+C to cancel)\n\n";

	auto mapToAudioLBA = [&](double fraction) -> DWORD {
		DWORD target = static_cast<DWORD>(totalSectors * fraction);
		DWORD cumulative = 0;
		for (const auto& [start, end] : audioRanges) {
			DWORD rangeLen = end - start + 1;
			if (cumulative + rangeLen > target) {
				return start + (target - cumulative);
			}
			cumulative += rangeLen;
		}
		return audioRanges.back().second;
		};

	constexpr int NUM_POSITIONS = 11;
	constexpr int REPEATS_PER_PAIR = 5;

	std::vector<DWORD> testPositions;
	for (int i = 0; i < NUM_POSITIONS; i++) {
		DWORD lba = mapToAudioLBA(static_cast<double>(i) / (NUM_POSITIONS - 1));
		// Deduplicate: only add if different from the last position
		if (testPositions.empty() || lba != testPositions.back()) {
			testPositions.push_back(lba);
		}
	}

	if (testPositions.size() < 2) {
		std::cout << "Not enough distinct positions to test.\n";
		return false;
	}

	std::vector<BYTE> buf(AUDIO_SECTOR_SIZE);
	m_drive.ReadSectorAudioOnly(testPositions.front(), buf.data());
	m_drive.ReadSectorAudioOnly(testPositions.back(), buf.data());

	int totalTests = static_cast<int>(testPositions.size() * (testPositions.size() - 1));
	ProgressIndicator progress(40);
	progress.SetLabel("  Seek Test");
	progress.Start();

	// Reuse timings vector across iterations
	std::vector<double> timings;
	timings.reserve(REPEATS_PER_PAIR);

	int tested = 0;
	for (size_t i = 0; i < testPositions.size(); i++) {
		for (size_t j = 0; j < testPositions.size(); j++) {
			if (i == j) continue;

			if (g_interrupt.IsInterrupted() || g_interrupt.CheckEscapeKey()) {
				std::cout << "\n\n*** Test cancelled by user ***\n";
				m_drive.SetSpeed(0);
				progress.Finish(false);
				return false;
			}

			DWORD fromLBA = testPositions[i];
			DWORD toLBA = testPositions[j];

			timings.clear();
			bool anyReadFailed = false;

			for (int rep = 0; rep < REPEATS_PER_PAIR; rep++) {
				// Defeat cache around both source and destination
				DefeatDriveCache(fromLBA, audioRanges.back().second);
				m_drive.ReadSectorAudioOnly(fromLBA, buf.data());
				DefeatDriveCache(toLBA, audioRanges.back().second);

				auto startTime = std::chrono::high_resolution_clock::now();
				bool readOk = m_drive.SeekToLBA(toLBA);
				auto endTime = std::chrono::high_resolution_clock::now();

				double seekMs = std::chrono::duration<double, std::milli>(endTime - startTime).count();
				timings.push_back(seekMs);
				if (!readOk) anyReadFailed = true;
			}

			std::sort(timings.begin(), timings.end());
			double medianSeekMs = timings[REPEATS_PER_PAIR / 2];

			SeekTimeResult r;
			r.fromLBA = fromLBA;
			r.toLBA = toLBA;
			r.seekTimeMs = medianSeekMs;
			r.abnormal = anyReadFailed;
			results.push_back(r);

			tested++;
			progress.Update(tested, totalTests);
		}
	}

	progress.Finish(true);
	m_drive.SetSpeed(0);
	m_drive.SpinDown();

	if (results.empty()) {
		std::cout << "No seek tests completed.\n";
		return false;
	}

	double sum = 0, maxSeek = 0;
	for (const auto& r : results) {
		sum += r.seekTimeMs;
		if (r.seekTimeMs > maxSeek) maxSeek = r.seekTimeMs;
	}
	double avgSeek = sum / results.size();

	double varianceSum = 0;
	for (const auto& r : results) {
		double diff = r.seekTimeMs - avgSeek;
		varianceSum += diff * diff;
	}
	double stddev = std::sqrt(varianceSum / (results.size() - 1));

	double abnormalThreshold = avgSeek + 3.0 * stddev;
	int abnormalCount = 0;
	for (auto& r : results) {
		if (r.seekTimeMs > abnormalThreshold || r.abnormal) {
			r.abnormal = true;
			abnormalCount++;
		}
	}

	std::cout << "\n" << std::string(60, '=') << "\n";
	std::cout << "              SEEK TIME ANALYSIS REPORT\n";
	std::cout << std::string(60, '=') << "\n";
	std::cout << "  (Measures head movement speed to detect mechanical or surface issues)\n\n";

	std::cout << "--- Timing Statistics ---\n";
	std::cout << "  Tests performed:  " << results.size() << "\n";
	std::cout << "  Average seek:     " << std::fixed << std::setprecision(1) << avgSeek << " ms";
	if (avgSeek < 80) std::cout << "  (normal)";
	else if (avgSeek < 150) std::cout << "  (slow - may indicate surface issues)";
	else std::cout << "  (very slow - possible mechanical problem)";
	std::cout << "\n";
	std::cout << "  Std deviation:    " << std::setprecision(1) << stddev << " ms";
	if (stddev > avgSeek * 0.5) std::cout << "  (high variability)";
	std::cout << "\n";
	std::cout << "  Maximum seek:     " << std::setprecision(1) << maxSeek << " ms\n";
	std::cout << "  Abnormal cutoff:  " << std::setprecision(1) << abnormalThreshold << " ms  (avg + 3 std dev)\n";
	if (abnormalCount > 0) {
		std::cout << "  Abnormal seeks:   " << abnormalCount;
		std::cout << " (" << std::setprecision(1) << (abnormalCount * 100.0 / results.size()) << "%)";
		std::cout << "  ** regions where head struggled to read **\n";
	}
	else {
		std::cout << "  Abnormal seeks:   None - consistent mechanical performance\n";
	}
	std::cout << std::string(60, '=') << "\n";

	return true;
}

// ── Disc Balance Check ──────────────────────────────────────────────────────
// Reads a fixed set of sample sectors at increasing speeds (4x to 40x).
// An unbalanced disc shows a sharp C2 error increase at higher RPMs.
bool AudioCDCopier::CheckDiscBalance(DiscInfo& disc, int& balanceScore) {
	if (!m_drive.CheckC2Support()) {
		std::cout << "ERROR: C2 support required for disc balance check.\n";
		return false;
	}

	const int speeds[] = { 4, 8, 16, 24, 32, 40 };
	const int NUM_SPEEDS = sizeof(speeds) / sizeof(speeds[0]);
	const int SAMPLE_COUNT = 50;

	DWORD totalSectors = CalculateTotalAudioSectors(disc);
	if (totalSectors == 0) return false;

	int sampleInterval = std::max(1, static_cast<int>(totalSectors / SAMPLE_COUNT));

	// Build a list of valid audio LBAs to sample (respecting track boundaries)
	std::vector<DWORD> sampleLBAs;
	DWORD maxLBA = 0;
	for (const auto& t : disc.tracks) {
		if (!t.isAudio) continue;
		DWORD start = (t.trackNumber == 1) ? 0 : t.pregapLBA;
		for (DWORD lba = start; lba <= t.endLBA; lba += sampleInterval) {
			sampleLBAs.push_back(lba);
		}
		if (t.endLBA > maxLBA) maxLBA = t.endLBA;
	}
	if (sampleLBAs.empty()) return false;

	int totalTests = NUM_SPEEDS * static_cast<int>(sampleLBAs.size());
	int completed = 0;

	std::cout << "\nSweeping " << sampleLBAs.size() << " sample sectors across "
		<< NUM_SPEEDS << " speeds...\n";
	std::cout << "  (Press ESC or Ctrl+C to cancel)\n\n";

	ProgressIndicator progress(40);
	progress.SetLabel("  Balance");
	progress.Start();

	std::vector<BYTE> buf(AUDIO_SECTOR_SIZE);
	std::vector<double> avgC2PerSpeed(NUM_SPEEDS, 0.0);

	for (int s = 0; s < NUM_SPEEDS; s++) {
		m_drive.SetSpeed(speeds[s]);
		Sleep(200); // Let the drive stabilize at new speed

		int totalC2 = 0, tested = 0;
		for (DWORD lba : sampleLBAs) {
			if (g_interrupt.IsInterrupted() || g_interrupt.CheckEscapeKey()) {
				std::cout << "\n\n*** Balance check cancelled by user ***\n";
				m_drive.SetSpeed(0);
				m_drive.SpinDown();
				progress.Finish(false);
				return false;
			}

			int c2 = 0;
			DefeatDriveCache(lba, maxLBA);
			if (m_drive.ReadSectorWithC2(lba, buf.data(), nullptr, c2)) {
				totalC2 += c2;
			}
			else {
				totalC2 += 100; // Penalize read failures
			}
			tested++;
			completed++;
			progress.Update(completed, totalTests);
		}
		avgC2PerSpeed[s] = (tested > 0) ? (double)totalC2 / tested : 0.0;
	}

	progress.Finish(true);
	m_drive.SetSpeed(0);
	m_drive.SpinDown();

	// Detect balance problem: compare low-speed baseline to high-speed errors
	double baseline = avgC2PerSpeed[0]; // 4x is the baseline
	double peakRatio = 0.0;
	for (int s = 1; s < NUM_SPEEDS; s++) {
		double ratio;
		if (baseline > 0.01) {
			ratio = avgC2PerSpeed[s] / baseline;
		}
		else {
			// Baseline is ~0: treat any high-speed errors as an absolute penalty
			ratio = 1.0 + avgC2PerSpeed[s];
		}
		if (ratio > peakRatio) peakRatio = ratio;
	}

	// Score: 100 = perfectly balanced, 0 = severe wobble
	if (peakRatio <= 1.5)       balanceScore = 100;
	else if (peakRatio <= 3.0)  balanceScore = 75;
	else if (peakRatio <= 8.0)  balanceScore = 50;
	else if (peakRatio <= 20.0) balanceScore = 25;
	else                        balanceScore = 0;

	// Report
	std::cout << "\n" << std::string(60, '=') << "\n";
	std::cout << "              DISC BALANCE CHECK\n";
	std::cout << std::string(60, '=') << "\n";
	std::cout << "  (Detects vibration / wobble by sweeping read speed)\n\n";

	std::cout << "--- C2 Errors by Speed ---\n";
	for (int s = 0; s < NUM_SPEEDS; s++) {
		std::cout << "  " << std::setw(3) << speeds[s] << "x:  "
			<< std::fixed << std::setprecision(2) << avgC2PerSpeed[s]
			<< " avg C2/sector\n";
	}
	std::cout << "\n  Balance Score: " << balanceScore << " / 100";
	if (balanceScore >= 75)      std::cout << "  (GOOD - disc is well balanced)\n";
	else if (balanceScore >= 50) std::cout << "  (FAIR - some wobble detected, reduce rip speed)\n";
	else                         std::cout << "  (POOR - significant balance problem, use 4x-8x max)\n";

	std::cout << std::string(60, '=') << "\n";
	return true;
}