#define NOMINMAX
#include "AudioCDCopier.h"
#include "InterruptHandler.h"
#include <iostream>
#include <iomanip>
#include <vector>
#include <algorithm>
#include <chrono>

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
	int totalSamples = static_cast<int>(totalSectors / sampleInterval) + 1;
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

			m_drive.SetSpeed(4);
			Sleep(100);
			lowOk = m_drive.ReadSectorWithC2(lba, buf.data(), nullptr, r.lowSpeedC2);

			DefeatDriveCache(lba, t.endLBA);

			m_drive.SetSpeed(24);
			Sleep(100);
			highOk = m_drive.ReadSectorWithC2(lba, buf.data(), nullptr, r.highSpeedC2);

			if (!lowOk) r.lowSpeedC2 = -1;
			if (!highOk) r.highSpeedC2 = -1;

			r.inconsistent =
				(!lowOk || !highOk) ||
				(r.highSpeedC2 > r.lowSpeedC2 * 2 && r.highSpeedC2 > 10) ||
				(r.lowSpeedC2 > r.highSpeedC2 * 2 && r.lowSpeedC2 > 10);

			if (r.lowSpeedC2 != 0 || r.highSpeedC2 != 0) results.push_back(r);
			tested++;
			progress.Update(tested, totalSamples);
		}
	}
	progress.Finish(true);
	m_drive.SetSpeed(0);

	int lowSpeedErrors = 0, highSpeedErrors = 0, inconsistentCount = 0;
	int lowFailures = 0, highFailures = 0;
	for (const auto& r : results) {
		if (r.lowSpeedC2 < 0) lowFailures++;
		else lowSpeedErrors += r.lowSpeedC2;
		if (r.highSpeedC2 < 0) highFailures++;
		else highSpeedErrors += r.highSpeedC2;
		if (r.inconsistent) inconsistentCount++;
	}

	std::cout << "\n=== Speed Test Results ===\n";
	std::cout << "Sectors tested:                    " << tested << "\n";
	std::cout << "Low speed  (4x)  total C2 errors:  " << lowSpeedErrors;
	if (lowFailures > 0) std::cout << " (+" << lowFailures << " read failures)";
	std::cout << "\n";
	std::cout << "High speed (24x) total C2 errors:  " << highSpeedErrors;
	if (highFailures > 0) std::cout << " (+" << highFailures << " read failures)";
	std::cout << "\n";
	std::cout << "Inconsistent sectors:              " << inconsistentCount << "\n\n";

	std::cout << "Recommended optimal speed: ";
	if (lowFailures > 0 || highFailures > 0) {
		std::cout << "2-4x (read failures detected - use slowest reliable speed)\n";
	}
	else if (results.empty() || (lowSpeedErrors == 0 && highSpeedErrors == 0)) {
		std::cout << "24x (disc reads cleanly at any speed)\n";
	}
	else if (highSpeedErrors > lowSpeedErrors * 2 || inconsistentCount > tested / 10) {
		std::cout << "4x (significant errors at high speed)\n";
	}
	else if (highSpeedErrors > lowSpeedErrors) {
		std::cout << "8-12x (moderate speed recommended)\n";
	}
	else {
		std::cout << "24x (no significant benefit from slower speeds)\n";
	}

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
	constexpr int REPEATS_PER_PAIR = 3;

	std::vector<DWORD> testPositions;
	for (int i = 0; i < NUM_POSITIONS; i++) {
		testPositions.push_back(mapToAudioLBA(static_cast<double>(i) / (NUM_POSITIONS - 1)));
	}

	std::vector<BYTE> buf(AUDIO_SECTOR_SIZE);
	m_drive.ReadSectorAudioOnly(testPositions.front(), buf.data());
	m_drive.ReadSectorAudioOnly(testPositions.back(), buf.data());

	int totalTests = static_cast<int>(testPositions.size() * (testPositions.size() - 1));
	ProgressIndicator progress(40);
	progress.SetLabel("  Seek Test");
	progress.Start();

	int tested = 0;
	for (size_t i = 0; i < testPositions.size(); i++) {
		for (size_t j = 0; j < testPositions.size(); j++) {
			if (i == j) continue;

			if (g_interrupt.IsInterrupted() || g_interrupt.CheckEscapeKey()) {
				m_drive.SetSpeed(0);
				progress.Finish(false);
				return false;
			}

			DWORD fromLBA = testPositions[i];
			DWORD toLBA = testPositions[j];

			std::vector<double> timings;
			timings.reserve(REPEATS_PER_PAIR);
			bool anyReadFailed = false;

			for (int rep = 0; rep < REPEATS_PER_PAIR; rep++) {
				m_drive.ReadSectorAudioOnly(fromLBA, buf.data());

				auto startTime = std::chrono::high_resolution_clock::now();
				bool readOk = m_drive.ReadSectorAudioOnly(toLBA, buf.data());
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
	double stddev = std::sqrt(varianceSum / results.size());

	double abnormalThreshold = avgSeek + 3.0 * stddev;
	int abnormalCount = 0;
	for (auto& r : results) {
		if (r.seekTimeMs > abnormalThreshold || r.abnormal) {
			r.abnormal = true;
			abnormalCount++;
		}
	}

	std::cout << "Tests performed: " << results.size() << "\n";
	std::cout << "Average seek time: " << std::fixed << std::setprecision(1) << avgSeek << " ms\n";
	std::cout << "Std deviation:     " << stddev << " ms\n";
	std::cout << "Maximum seek time: " << maxSeek << " ms\n";
	std::cout << "Abnormal threshold:" << abnormalThreshold << " ms\n";
	if (abnormalCount > 0)
		std::cout << "Abnormal seeks:    " << abnormalCount << "\n";

	return true;
}