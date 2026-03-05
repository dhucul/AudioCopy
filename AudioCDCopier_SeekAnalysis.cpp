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
// Seek Time Analysis - Measures head movement speed to detect mechanical issues
// ============================================================================

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