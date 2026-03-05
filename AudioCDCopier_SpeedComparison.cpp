#define NOMINMAX
#include "AudioCDCopier.h"
#include "InterruptHandler.h"
#include <iostream>
#include <iomanip>
#include <vector>
#include <algorithm>

// ============================================================================
// Speed Comparison Test - Compares read errors at slow vs fast speed
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
	progress.Finish(true); m_drive.SetSpeed(0);
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