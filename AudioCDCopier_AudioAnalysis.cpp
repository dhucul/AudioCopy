#define NOMINMAX
#include "AudioCDCopier.h"
#include "InterruptHandler.h"
#include <iostream>
#include <iomanip>
#include <vector>
#include <cstdlib>

// ============================================================================
// Audio Content Analysis
// ============================================================================

bool AudioCDCopier::AnalyzeAudioContent(DiscInfo& disc, AudioAnalysisResult& result, int scanSpeed) {
	std::cout << "\n=== Audio Content Analysis ===\n";
	result = AudioAnalysisResult{};
	m_drive.SetSpeed(scanSpeed);

	DWORD totalSectors = CalculateTotalAudioSectors(disc);
	if (totalSectors == 0) {
		std::cout << "No audio tracks to analyze.\n";
		return false;
	}

	int sampleInterval = 100;
	int totalSamples = static_cast<int>(totalSectors / sampleInterval) + 1;
	std::cout << "Sampling ~" << totalSamples << " sectors (every " << sampleInterval << ")...\n";
	std::cout << "  (Press ESC or Ctrl+C to cancel)\n\n";

	ProgressIndicator progress(40);
	progress.SetLabel("  Audio Analysis");
	progress.Start();

	std::vector<BYTE> buf(AUDIO_SECTOR_SIZE);
	int tested = 0;
	int readFailures = 0;

	for (const auto& t : disc.tracks) {
		if (!t.isAudio) continue;
		DWORD start = (t.trackNumber == 1) ? 0 : t.pregapLBA;

		for (DWORD lba = start; lba <= t.endLBA; lba += sampleInterval) {
			if (g_interrupt.IsInterrupted() || g_interrupt.CheckEscapeKey()) {
				std::cout << "\n\n*** Analysis cancelled by user ***\n";
				m_drive.SetSpeed(0);
				progress.Finish(false);
				return false;
			}

			if (!m_drive.ReadSectorAudioOnly(lba, buf.data())) {
				readFailures++;
				tested++;
				progress.Update(tested, totalSamples);
				continue;
			}

			bool suspicious = false;

			if (IsSectorSilent(buf.data())) {
				result.silentSectors++;
			}
			if (IsSectorClipped(buf.data())) {
				result.clippedSectors++;
				suspicious = true;
			}

			bool lowLevel = true;
			for (int i = 0; i < AUDIO_SECTOR_SIZE; i += 2) {
				int16_t sample = *reinterpret_cast<const int16_t*>(buf.data() + i);
				if (std::abs(sample) > 327) {
					lowLevel = false;
					break;
				}
			}
			if (lowLevel && !IsSectorSilent(buf.data())) {
				result.lowLevelSectors++;
			}

			int64_t sampleSum = 0;
			int sampleCount = AUDIO_SECTOR_SIZE / 2;
			for (int i = 0; i < AUDIO_SECTOR_SIZE; i += 2) {
				sampleSum += *reinterpret_cast<const int16_t*>(buf.data() + i);
			}
			double avgSample = static_cast<double>(sampleSum) / sampleCount;
			if (std::abs(avgSample) > 500.0) {
				result.dcOffsetSectors++;
				suspicious = true;
			}

			if (suspicious) {
				result.suspiciousLBAs.push_back(lba);
			}

			tested++;
			progress.Update(tested, totalSamples);
		}
	}

	progress.Finish(true);
	m_drive.SetSpeed(0);

	std::cout << "\n" << std::string(60, '=') << "\n";
	std::cout << "           AUDIO CONTENT ANALYSIS REPORT\n";
	std::cout << std::string(60, '=') << "\n";

	std::cout << "\n--- Scan Summary ---\n";
	std::cout << "  Sectors sampled:   " << tested << "\n";
	std::cout << "  Read failures:     " << readFailures << "\n";

	std::cout << "\n--- Content Issues ---\n";
	std::cout << "  Silent sectors:    " << result.silentSectors;
	if (tested > 0)
		std::cout << " (" << std::fixed << std::setprecision(1)
		<< (result.silentSectors * 100.0 / tested) << "%)";
	std::cout << "\n";

	std::cout << "  Clipped sectors:   " << result.clippedSectors;
	if (tested > 0)
		std::cout << " (" << std::fixed << std::setprecision(1)
		<< (result.clippedSectors * 100.0 / tested) << "%)";
	std::cout << "\n";

	std::cout << "  Low-level sectors: " << result.lowLevelSectors << "\n";
	std::cout << "  DC offset sectors: " << result.dcOffsetSectors << "\n";

	if (!result.suspiciousLBAs.empty()) {
		int showCount = std::min(10, static_cast<int>(result.suspiciousLBAs.size()));
		std::cout << "\n--- Suspicious Sectors (showing " << showCount << " of "
			<< result.suspiciousLBAs.size() << ") ---\n";
		for (int i = 0; i < showCount; i++) {
			std::cout << "  LBA " << result.suspiciousLBAs[i] << "\n";
		}
	}

	std::cout << std::string(60, '=') << "\n";
	return true;
}

bool AudioCDCopier::IsSectorSilent(const BYTE* data) {
	for (int i = 0; i < AUDIO_SECTOR_SIZE; i += 4) {
		int16_t left = *reinterpret_cast<const int16_t*>(data + i);
		int16_t right = *reinterpret_cast<const int16_t*>(data + i + 2);
		if (std::abs(left) > 10 || std::abs(right) > 10) return false;
	}
	return true;
}

bool AudioCDCopier::IsSectorClipped(const BYTE* data) {
	int clippedSamples = 0;
	int totalSamples = AUDIO_SECTOR_SIZE / 2;
	for (int i = 0; i < AUDIO_SECTOR_SIZE; i += 2) {
		int16_t sample = *reinterpret_cast<const int16_t*>(data + i);
		if (sample == 32767 || sample == -32768) clippedSamples++;
	}
	return clippedSamples > (totalSamples / 100);
}