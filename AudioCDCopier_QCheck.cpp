#define NOMINMAX
#include "AudioCDCopier.h"
#include "InterruptHandler.h"
#include "ConsoleGraph.h"
#include <iostream>
#include <iomanip>
#include <sstream>
#include <thread>
#include <chrono>
#include <algorithm>
#include <functional>
#include <fstream>

// ============================================================================
// Plextor Q-Check Quality Scan (0xE9 / 0xEB vendor commands)
// ============================================================================
// This is fundamentally different from the D8-based BLER scan.  The drive
// enters a dedicated error-measurement mode: it spins at ~1x and reports
// aggregate CIRC decoder statistics (C1/C2/CU) per time slice without
// transferring any audio data.  This matches QPXTool's C1/C2 scan.
// ============================================================================

bool AudioCDCopier::RunQCheckScan(const DiscInfo& disc, QCheckResult& result, int scanSpeed) {
	std::cout << "\n=== CD Quality Scan (C1/C2/CU) ===\n";

	// ── Check Plextor Q-Check first, then LiteOn scan ────────
	bool usePlextor = m_drive.SupportsQCheck();
	bool useLiteOn = false;

	if (!usePlextor) {
		useLiteOn = m_drive.SupportsLiteOnScan();
	}

	if (!usePlextor && !useLiteOn) {
		std::cout << "ERROR: No quality scan commands available on this drive.\n";
		std::cout << "       Plextor Q-Check (0xE9/0xEB): not supported\n";
		std::cout << "       LiteOn/MediaTek (0xDF):      not supported\n";
		result.supported = false;
		return false;
	}

	result.supported = true;

	if (usePlextor)
		std::cout << "Using Plextor Q-Check (0xE9/0xEB)\n";
	else
		std::cout << "Using LiteOn/MediaTek quality scan (0xDF)\n";

	// ── Determine scan range ─────────────────────────────────
	DWORD firstLBA = 0, lastLBA = 0;
	bool first = true;
	for (const auto& t : disc.tracks) {
		if (t.isAudio) {
			DWORD start = (t.trackNumber == 1) ? 0 : t.pregapLBA;
			if (first) { firstLBA = start; first = false; }
			lastLBA = t.endLBA;
		}
	}

	if (first) {
		std::cout << "No audio tracks found.\n";
		return false;
	}

	result.totalSectors = lastLBA - firstLBA + 1;
	result.totalSeconds = (result.totalSectors + 74) / 75;

	std::cout << "Scan range: LBA " << firstLBA << " - " << lastLBA
		<< " (" << result.totalSectors << " sectors, ~"
		<< result.totalSeconds << " sec)\n";
	if (usePlextor)
		std::cout << "Drive will scan at ~1x internally (hardware-driven).\n";
	std::cout << "  (Press ESC or Ctrl+C to cancel)\n\n";

	// ── Start the hardware scan ──────────────────────────────
	if (useLiteOn) {
		// LiteOn scans at current drive speed — use selected scan speed
		m_drive.SetSpeed(scanSpeed);
		if (scanSpeed == 0)
			std::cout << "Scan speed: Max\n";
		else
			std::cout << "Scan speed: " << scanSpeed << "x\n";
	}

	bool started = usePlextor
		? m_drive.PlextorQCheckStart(firstLBA, lastLBA)
		: m_drive.LiteOnScanStart(firstLBA, lastLBA);

	if (!started) {
		std::cout << "ERROR: Failed to start quality scan.\n";
		return false;
	}

	// ── Poll for results ─────────────────────────────────────
	bool scanDone = false;
	int sampleIndex = 0;
	DWORD lastReportedLBA = DWORD(-1);

	// ── Timer state for elapsed / ETA display ────────────────
	auto scanStartTime = std::chrono::steady_clock::now();
	int lastLineLength = 0;
	constexpr int BAR_WIDTH = 25;

	while (!scanDone) {
		if (InterruptHandler::Instance().IsInterrupted() || InterruptHandler::Instance().CheckEscapeKey()) {
			if (usePlextor) m_drive.PlextorQCheckStop();
			else m_drive.LiteOnScanStop();
			std::cout << "\n*** Quality scan cancelled by user ***\n";
			return false;
		}

		// Plextor Q-Check is async (drive scans internally) — needs polling delay.
		// LiteOn is synchronous (each call reads one block) — no delay needed.
		if (usePlextor)
			std::this_thread::sleep_for(std::chrono::milliseconds(500));

		int c1 = 0, c2 = 0, cu = 0;
		DWORD currentLBA = 0;

		bool pollOk = usePlextor
			? m_drive.PlextorQCheckPoll(c1, c2, cu, currentLBA, scanDone)
			: m_drive.LiteOnScanPoll(c1, c2, cu, currentLBA, scanDone);

		if (!pollOk) {
			if (usePlextor) {
				std::this_thread::sleep_for(std::chrono::milliseconds(200));
				pollOk = m_drive.PlextorQCheckPoll(c1, c2, cu, currentLBA, scanDone);
			}
			if (!pollOk) {
				if (usePlextor) m_drive.PlextorQCheckStop();
				else m_drive.LiteOnScanStop();
				// If we already have samples, treat as completed scan
				if (!result.samples.empty()) break;
				std::cout << "\nERROR: Lost communication with drive during scan.\n";
				return false;
			}
		}

		if (currentLBA == 0 && c1 == 0 && c2 == 0 && cu == 0 && !scanDone)
			continue;

		if (currentLBA == lastReportedLBA && !scanDone)
			continue;
		lastReportedLBA = currentLBA;

		// Discard the first few samples — the drive is still seeking/spinning
		// up and reports accumulated startup errors in the initial responses.
		// QPXTool does the same; without this the first sample creates a
		// massive spike that dominates the entire graph and skews statistics.
		// Applies to both Plextor and LiteOn paths.
		if (sampleIndex < 3) {
			sampleIndex++;
			continue;
		}

		// LiteOn: detect end of disc by LBA reaching scan range
		if (useLiteOn && currentLBA >= lastLBA) {
			scanDone = true;
		}

		QCheckSample sample;
		sample.lba = currentLBA;
		sample.c1 = c1;
		sample.c2 = c2;
		sample.cu = cu;
		result.samples.push_back(sample);

		result.totalC1 += c1;
		result.totalC2 += c2;
		result.totalCU += cu;

		int idx = static_cast<int>(result.samples.size()) - 1;

		if (c1 > result.maxC1PerSecond) {
			result.maxC1PerSecond = c1;
			result.maxC1SecondIndex = idx;
		}
		if (c2 > result.maxC2PerSecond) {
			result.maxC2PerSecond = c2;
			result.maxC2SecondIndex = idx;
		}
		if (cu > result.maxCUPerSecond)
			result.maxCUPerSecond = cu;

		sampleIndex++;

		// ── Compute progress, elapsed, and ETA ───────────────
		double pct = 0.0;
		if (currentLBA >= firstLBA && result.totalSectors > 0) {
			pct = static_cast<double>(currentLBA - firstLBA) * 100.0 / result.totalSectors;
			if (pct > 100.0) pct = 100.0;
		}

		auto now = std::chrono::steady_clock::now();
		int elapsedSec = static_cast<int>(
			std::chrono::duration_cast<std::chrono::seconds>(now - scanStartTime).count());

		// ETA: extrapolate from elapsed time and progress fraction
		int etaSec = -1;
		if (pct > 1.0 && pct < 100.0) {
			double remaining = (100.0 - pct) / pct;
			etaSec = static_cast<int>(elapsedSec * remaining);
		}

		// ── Format progress bar ──────────────────────────────
		int filled = static_cast<int>(pct * BAR_WIDTH / 100.0);
		std::ostringstream line;
		line << "\r  [";
		for (int i = 0; i < BAR_WIDTH; i++)
			line << (i < filled ? "\xe2\x96\x88" : "\xe2\x96\x91"); // █ or ░
		line << "] " << std::fixed << std::setprecision(1) << pct << "%";

		// Elapsed time
		line << "  " << (elapsedSec / 60) << ":"
			<< std::setfill('0') << std::setw(2) << (elapsedSec % 60);

		// ETA
		if (etaSec >= 0) {
			line << " ETA:";
			if (etaSec >= 3600)
				line << (etaSec / 3600) << "h"
				<< std::setfill('0') << std::setw(2) << ((etaSec % 3600) / 60) << "m"
				<< std::setfill('0') << std::setw(2) << (etaSec % 60) << "s";
			else if (etaSec >= 60)
				line << (etaSec / 60) << "m"
				<< std::setfill('0') << std::setw(2) << (etaSec % 60) << "s";
			else
				line << etaSec << "s";
		}

		// Current error counts
		line << "  C1=" << c1 << " C2=" << c2 << " CU=" << cu;

		// Pad to clear previous line remnants
		std::string output = line.str();
		if (static_cast<int>(output.size()) < lastLineLength)
			output.append(lastLineLength - output.size(), ' ');
		lastLineLength = static_cast<int>(output.size());

		std::cout << output << std::flush;
	}

	// ── Print final elapsed time ─────────────────────────────
	auto scanEndTime = std::chrono::steady_clock::now();
	int totalElapsed = static_cast<int>(
		std::chrono::duration_cast<std::chrono::seconds>(scanEndTime - scanStartTime).count());

	std::ostringstream elapsed;
	if (totalElapsed >= 3600)
		elapsed << (totalElapsed / 3600) << "h "
		<< std::setfill('0') << std::setw(2) << ((totalElapsed % 3600) / 60) << "m "
		<< std::setfill('0') << std::setw(2) << (totalElapsed % 60) << "s";
	else if (totalElapsed >= 60)
		elapsed << (totalElapsed / 60) << "m "
		<< std::setfill('0') << std::setw(2) << (totalElapsed % 60) << "s";
	else
		elapsed << totalElapsed << "s";

	std::cout << "\n  Done in " << elapsed.str()
		<< " (" << result.samples.size() << " samples)\n";

	// ── Remove startup spike(s) ──────────────────────────────
	// Even after discarding initial warm-up polls, the drive may dump
	// accumulated seek/spin-up errors into any of the first several
	// recorded samples — timing varies per drive model and disc.
	// Use the median error rate (immune to the spike itself) as the
	// baseline, and scan a generous leading window.
	if (result.samples.size() > 50) {
		// Median of all samples — robust against any single outlier
		std::vector<int> allErrs;
		allErrs.reserve(result.samples.size());
		for (const auto& s : result.samples)
			allErrs.push_back(s.c1 + s.c2 + s.cu);
		std::sort(allErrs.begin(), allErrs.end());
		int median = allErrs[allErrs.size() / 2];

		// Check up to the first 30 samples (~15 sec at 500 ms polling),
		// but never more than half the dataset for very short scans.
		size_t checkEnd = std::min<size_t>(30, result.samples.size() / 2);

		bool trimmed = false;
		for (int i = static_cast<int>(checkEnd) - 1; i >= 0; i--) {
			int err = result.samples[i].c1 + result.samples[i].c2 + result.samples[i].cu;
			if ((median == 0 && err > 10) || (median > 0 && err > median * 10)) {
				result.totalC1 -= result.samples[i].c1;
				result.totalC2 -= result.samples[i].c2;
				result.totalCU -= result.samples[i].cu;
				result.samples.erase(result.samples.begin() + i);
				trimmed = true;
			}
		}

		if (trimmed) {
			result.maxC1PerSecond = 0; result.maxC1SecondIndex = -1;
			result.maxC2PerSecond = 0; result.maxC2SecondIndex = -1;
			result.maxCUPerSecond = 0;
			for (int i = 0; i < static_cast<int>(result.samples.size()); i++) {
				const auto& s = result.samples[i];
				if (s.c1 > result.maxC1PerSecond) { result.maxC1PerSecond = s.c1; result.maxC1SecondIndex = i; }
				if (s.c2 > result.maxC2PerSecond) { result.maxC2PerSecond = s.c2; result.maxC2SecondIndex = i; }
				if (s.cu > result.maxCUPerSecond) result.maxCUPerSecond = s.cu;
			}
		}
	}

	// ── C2 recheck: verify transient C2 errors ───────────────
	// Plextor drives occasionally report spurious C2 errors that vanish
	// on a second scan.  When C2 errors are found, automatically re-scan
	// and compare.  If the recheck is clean, the original C2 counts are
	// discarded as transient hardware artifacts.
	if (result.totalC2 > 0 && !InterruptHandler::Instance().IsInterrupted()) {
		std::cout << "\n  C2 errors detected (" << result.totalC2
			<< " total) — running verification re-scan...\n";

		// Stop any lingering scan state before restarting
		if (usePlextor) m_drive.PlextorQCheckStop();
		else m_drive.LiteOnScanStop();
		std::this_thread::sleep_for(std::chrono::milliseconds(1000));

		bool recheckStarted = usePlextor
			? m_drive.PlextorQCheckStart(firstLBA, lastLBA)
			: m_drive.LiteOnScanStart(firstLBA, lastLBA);

		if (recheckStarted) {
			int recheckC2Total = 0;
			bool recheckDone = false;
			bool recheckStopped = false;       // ← track whether stop was already called
			int recheckSampleIdx = 0;
			DWORD recheckLastLBA = DWORD(-1);
			int recheckLastLine = 0;
			auto recheckStart = std::chrono::steady_clock::now();

			while (!recheckDone) {
				if (InterruptHandler::Instance().IsInterrupted() || InterruptHandler::Instance().CheckEscapeKey()) {
					if (usePlextor) m_drive.PlextorQCheckStop();
					else m_drive.LiteOnScanStop();
					recheckStopped = true;     // ← mark stopped
					std::cout << "\n  *** Recheck cancelled — keeping original C2 results ***\n";
					break;
				}

				if (usePlextor)
					std::this_thread::sleep_for(std::chrono::milliseconds(500));

				int rc1 = 0, rc2 = 0, rcu = 0;
				DWORD rLBA = 0;

				bool rpoll = usePlextor
					? m_drive.PlextorQCheckPoll(rc1, rc2, rcu, rLBA, recheckDone)
					: m_drive.LiteOnScanPoll(rc1, rc2, rcu, rLBA, recheckDone);

				if (!rpoll) {
					if (usePlextor) {
						std::this_thread::sleep_for(std::chrono::milliseconds(200));
						rpoll = m_drive.PlextorQCheckPoll(rc1, rc2, rcu, rLBA, recheckDone);
					}
					if (!rpoll) {
						if (usePlextor) m_drive.PlextorQCheckStop();
						else m_drive.LiteOnScanStop();
						recheckStopped = true; // ← mark stopped
						std::cout << "\n  Recheck communication lost — keeping original C2 results.\n";
						break;
					}
				}

				if (rLBA == 0 && rc1 == 0 && rc2 == 0 && rcu == 0 && !recheckDone)
					continue;
				if (rLBA == recheckLastLBA && !recheckDone)
					continue;
				recheckLastLBA = rLBA;

				// Skip startup samples just like the primary scan
				if (recheckSampleIdx < 3) {
					recheckSampleIdx++;
					continue;
				}

				if (useLiteOn && rLBA >= lastLBA)
					recheckDone = true;

				recheckC2Total += rc2;
				recheckSampleIdx++;

				// ── Recheck progress bar ─────────────────────
				double rpct = 0.0;
				if (rLBA >= firstLBA && result.totalSectors > 0) {
					rpct = static_cast<double>(rLBA - firstLBA) * 100.0 / result.totalSectors;
					if (rpct > 100.0) rpct = 100.0;
				}

				auto rnow = std::chrono::steady_clock::now();
				int rElapsed = static_cast<int>(
					std::chrono::duration_cast<std::chrono::seconds>(rnow - recheckStart).count());

				std::ostringstream rline;
				int rfilled = static_cast<int>(rpct * BAR_WIDTH / 100.0);
				rline << "\r  Recheck [";
				for (int i = 0; i < BAR_WIDTH; i++)
					rline << (i < rfilled ? "\xe2\x96\x88" : "\xe2\x96\x91");
				rline << "] " << std::fixed << std::setprecision(1) << rpct << "%"
					<< "  " << (rElapsed / 60) << ":"
					<< std::setfill('0') << std::setw(2) << (rElapsed % 60)
					<< "  C2=" << recheckC2Total;

				std::string routput = rline.str();
				if (static_cast<int>(routput.size()) < recheckLastLine)
					routput.append(recheckLastLine - routput.size(), ' ');
				recheckLastLine = static_cast<int>(routput.size());
				std::cout << routput << std::flush;
			}

			if (recheckDone) {
				if (recheckC2Total == 0) {
					// Recheck found no C2 errors — original was transient
					std::cout << "\n  Recheck PASSED: 0 C2 errors on re-scan.\n";
					std::cout << "  Original C2 count (" << result.totalC2
						<< ") discarded as transient.\n";

					// Clear C2 from each sample and recalculate totals
					for (auto& s : result.samples) {
						s.c2 = 0;
					}
					result.totalC2 = 0;
					result.maxC2PerSecond = 0;
					result.maxC2SecondIndex = -1;
				}
				else {
					// Recheck also found C2 errors — they are real
					std::cout << "\n  Recheck CONFIRMED: " << recheckC2Total
						<< " C2 errors on re-scan (original: " << result.totalC2 << ").\n";
					std::cout << "  C2 errors are genuine — keeping original results.\n";
				}
			}

			// Only stop if not already stopped by cancel/comm-loss handler
			if (!recheckStopped) {
				if (usePlextor) m_drive.PlextorQCheckStop();
				else m_drive.LiteOnScanStop();
			}
		}
		else {
			std::cout << "  WARNING: Could not start recheck scan — keeping original C2 results.\n";
		}
	}

	// ── Compute averages ─────────────────────────────────────
	DWORD sampleCount = static_cast<DWORD>(result.samples.size());
	if (sampleCount > 0) {
		result.avgC1PerSecond = static_cast<double>(result.totalC1) / sampleCount;
		result.avgC2PerSecond = static_cast<double>(result.totalC2) / sampleCount;
	}

	// ── Quality assessment (Red Book: avg C1 < 220/sec) ──────
	if (result.totalCU > 0)
		result.qualityRating = "BAD";
	else if (result.totalC2 > 0 && result.avgC2PerSecond > 10.0)
		result.qualityRating = "POOR";
	else if (result.totalC2 > 0)
		result.qualityRating = "FAIR";
	else if (result.avgC1PerSecond >= 220.0)
		result.qualityRating = "POOR";
	else if (result.avgC1PerSecond >= 50.0)
		result.qualityRating = "FAIR";
	else if (result.avgC1PerSecond >= 5.0)
		result.qualityRating = "GOOD";
	else
		result.qualityRating = "EXCELLENT";

	// ── Total C1 quality interpretation ──────────────────────
	if (result.totalC1 <= 500)
		result.totalC1Quality = "Exceptional";
	else if (result.totalC1 <= 5000)
		result.totalC1Quality = "Very good";
	else if (result.totalC1 <= 20000)
		result.totalC1Quality = "Normal";
	else if (result.totalC1 <= 50000)
		result.totalC1Quality = "Marginal";
	else
		result.totalC1Quality = "Poor";

	// ── Archival audio peak C1 assessment ────────────────────
	if (result.maxC1PerSecond < 50)
		result.archivalC1Rating = "Ideal";
	else if (result.maxC1PerSecond < 100)
		result.archivalC1Rating = "Good";
	else if (result.maxC1PerSecond <= 220)
		result.archivalC1Rating = "Acceptable";
	else
		result.archivalC1Rating = "Poor";

	PrintQCheckReport(result);
	return true;
}

void AudioCDCopier::PrintQCheckReport(const QCheckResult& result) {
	std::cout << "\n" << std::string(60, '=') << "\n";
	std::cout << "         PLEXTOR Q-CHECK QUALITY SCAN REPORT\n";
	std::cout << std::string(60, '=') << "\n";

	std::cout << "\n--- Scan Info ---\n";
	std::cout << "  Samples collected: " << result.samples.size() << "\n";
	std::cout << "  Disc length:       "
		<< (result.totalSeconds / 60) << ":"
		<< std::setfill('0') << std::setw(2) << (result.totalSeconds % 60)
		<< std::setfill(' ') << " (mm:ss)\n";
	std::cout << "  Sectors covered:   " << result.totalSectors << "\n";

	// ── C1 (BLER) ────────────────────────────────────────────
	std::cout << "\n--- C1 Errors (Block Error Rate) ---\n";
	std::cout << "  Total C1:    " << result.totalC1 << "\n";
	std::cout << "  Avg C1/sec:  " << std::fixed << std::setprecision(2)
		<< result.avgC1PerSecond;
	std::cout << (result.avgC1PerSecond < 220.0 ? "  [PASS]" : "  [FAIL]")
		<< "  (Red Book limit: 220/sec)\n";
	std::cout << "  Max C1/sec:  " << result.maxC1PerSecond;
	if (result.maxC1SecondIndex >= 0 && result.maxC1SecondIndex < static_cast<int>(result.samples.size()))
		std::cout << "  (at LBA " << result.samples[result.maxC1SecondIndex].lba << ")";

	// Report if there's a large discrepancy between max C1 and average C1
	if (result.samples.size() >= 10 && result.avgC1PerSecond > 50.0) {
		int warningMargin = 5; // allow some margin for minor fluctuations
		// Look for recent samples around the supposed max sample time
		for (int i = static_cast<int>(result.samples.size()) - 1; i >= 0; i--) {
			if (result.samples[i].lba < result.samples.back().lba - warningMargin)
				break;
			if (result.samples[i].c1 > result.avgC1PerSecond * 10) {
				std::cout << "  WARNING: Recent C1 spike detected (LBA "
					<< result.samples[i].lba << ", C1=" << result.samples[i].c1 << ")\n";
				break;
			}
		}
	}
	std::cout << "\n";

	if (result.avgC1PerSecond < 5.0)
		std::cout << "  C1 Assessment: EXCELLENT — minimal correction needed\n";
	else if (result.avgC1PerSecond < 50.0)
		std::cout << "  C1 Assessment: GOOD — normal wear\n";
	else if (result.avgC1PerSecond < 220.0)
		std::cout << "  C1 Assessment: FAIR — elevated but within Red Book limits\n";
	else
		std::cout << "  C1 Assessment: POOR — exceeds Red Book BLER limit\n";

	// ── Total C1 Quality ─────────────────────────────────────
	std::cout << "\n--- Total C1 Quality ---\n";
	std::cout << "  Total C1:      " << result.totalC1 << "\n";
	std::cout << "  Interpretation: ";
	if (result.totalC1Quality == "Exceptional" || result.totalC1Quality == "Very good")
		Console::SetColorRGB(Console::Theme::GreenR, Console::Theme::GreenG, Console::Theme::GreenB);
	else if (result.totalC1Quality == "Normal" || result.totalC1Quality == "Marginal")
		Console::SetColorRGB(Console::Theme::YellowR, Console::Theme::YellowG, Console::Theme::YellowB);
	else
		Console::SetColorRGB(Console::Theme::RedR, Console::Theme::RedG, Console::Theme::RedB);
	std::cout << result.totalC1Quality;
	Console::Reset();
	if (result.totalC1Quality == "Exceptional")
		std::cout << " (rare, usually high-quality pressings)";
	else if (result.totalC1Quality == "Marginal")
		std::cout << " (still within spec)";
	else if (result.totalC1Quality == "Poor")
		std::cout << " (poor burn or aging disc)";
	std::cout << "\n";

	// ── Archival Audio Peak C1 ───────────────────────────────
	std::cout << "\n--- Archival Audio (Peak C1) ---\n";
	std::cout << "  Peak C1/sec: " << result.maxC1PerSecond << "\n";
	std::cout << "  Rating:      ";
	if (result.archivalC1Rating == "Ideal" || result.archivalC1Rating == "Good")
		Console::SetColorRGB(Console::Theme::GreenR, Console::Theme::GreenG, Console::Theme::GreenB);
	else if (result.archivalC1Rating == "Acceptable")
		Console::SetColorRGB(Console::Theme::YellowR, Console::Theme::YellowG, Console::Theme::YellowB);
	else
		Console::SetColorRGB(Console::Theme::RedR, Console::Theme::RedG, Console::Theme::RedB);
	std::cout << result.archivalC1Rating;
	Console::Reset();
	if (result.archivalC1Rating == "Ideal")
		std::cout << " (< 50/sec)";
	else if (result.archivalC1Rating == "Good")
		std::cout << " (< 100/sec)";
	else if (result.archivalC1Rating == "Acceptable")
		std::cout << " (100-220/sec, not ideal for archival)";
	else
		std::cout << " (exceeds Red Book limit)";
	std::cout << "\n";

	// ── C2 (E22) ─────────────────────────────────────────────
	std::cout << "\n--- C2 Errors ---\n";
	std::cout << "  Total C2:    " << result.totalC2 << "\n";
	std::cout << "  Avg C2/sec:  " << std::fixed << std::setprecision(2)
		<< result.avgC2PerSecond << "\n";
	std::cout << "  Max C2/sec:  " << result.maxC2PerSecond;
	if (result.maxC2SecondIndex >= 0 && result.maxC2SecondIndex < static_cast<int>(result.samples.size()))
		std::cout << "  (at LBA " << result.samples[result.maxC2SecondIndex].lba << ")";

	// Brief C2 description: count, average, and max spikes
	if (result.totalC2 > 0) {
		if (result.avgC2PerSecond < 1.0)
			std::cout << "  (few, isolated spikes)";
		else if (result.avgC2PerSecond < 10.0)
			std::cout << "  (moderate spike activity)";
		else
			std::cout << "  (significant, sustained errors)";
	}
	std::cout << "\n";

	if (result.totalC2 == 0)
		std::cout << "  C2 Assessment: PERFECT — no C2 correction needed\n";
	else if (result.avgC2PerSecond < 1.0)
		std::cout << "  C2 Assessment: ACCEPTABLE — few C2 corrections (C1 fallthrough)\n";
	else if (result.avgC2PerSecond < 10.0)
		std::cout << "  C2 Assessment: FAIR — moderate C2 correction load\n";
	else
		std::cout << "  C2 Assessment: POOR — heavy C2 correction load\n";

	// ── CU (Uncorrectable) ───────────────────────────────────
	std::cout << "\n--- CU (Uncorrectable) ---\n";
	std::cout << "  Total CU:    " << result.totalCU << "\n";
	std::cout << "  Max CU/sec:  " << result.maxCUPerSecond << "\n";
	if (result.totalCU == 0)
		std::cout << "  CU Assessment: PERFECT — all errors were correctable\n";
	else
		std::cout << "  CU Assessment: BAD — data loss likely in " << result.totalCU << " sector(s)\n";

	// ── Distribution Graphs ──────────────────────────────────
	if (!result.samples.empty()) {
		constexpr int GRAPH_WIDTH = 60;
		constexpr int GRAPH_HEIGHT = 12;

		// Extract per-sample values
		std::vector<int> c1Vals, c2Vals, cuVals;
		for (const auto& s : result.samples) {
			c1Vals.push_back(s.c1);
			c2Vals.push_back(s.c2);
			cuVals.push_back(s.cu);
		}

		// ── C1 Error Distribution ──
		int peakC1 = *std::max_element(c1Vals.begin(), c1Vals.end());
		if (peakC1 > 0) {
			int graphMax = std::max(peakC1, 250); // ensure Red Book ref line is visible
			auto buckets = Console::BucketData(c1Vals, GRAPH_WIDTH);
			Console::GraphOptions opts;
			opts.title = "C1 Error Distribution (BLER)";
			opts.subtitle = "Each column = a time slice; height = C1 errors/sec";
			opts.width = GRAPH_WIDTH;
			opts.height = GRAPH_HEIGHT;
			opts.refLine = 220;
			opts.refLabel = "Red Book BLER limit (220/sec)";
			Console::DrawBarGraph(buckets, graphMax, opts, result.totalSeconds);
		}

		// ── C2 Error Distribution ──
		int peakC2 = *std::max_element(c2Vals.begin(), c2Vals.end());
		if (peakC2 > 0) {
			auto buckets = Console::BucketData(c2Vals, GRAPH_WIDTH);
			Console::GraphOptions opts;
			opts.title = "C2 Error Distribution";
			opts.subtitle = "Each column = a time slice; height = C2 errors/sec";
			opts.width = GRAPH_WIDTH;
			opts.height = GRAPH_HEIGHT;
			Console::DrawBarGraph(buckets, peakC2, opts, result.totalSeconds);
		}
		else {
			Console::SetColorRGB(Console::Theme::GreenR, Console::Theme::GreenG, Console::Theme::GreenB);
			std::cout << "\n  " << Console::Sym::Check << " No C2 errors.\n";
			Console::Reset();
		}

		// ── CU Error Distribution ──
		int peakCU = *std::max_element(cuVals.begin(), cuVals.end());
		if (peakCU > 0) {
			auto buckets = Console::BucketData(cuVals, GRAPH_WIDTH);
			Console::GraphOptions opts;
			opts.title = "CU (Uncorrectable) Distribution";
			opts.subtitle = "Each column = a time slice; height = CU events/sec";
			opts.width = GRAPH_WIDTH;
			opts.height = GRAPH_HEIGHT;
			Console::DrawBarGraph(buckets, peakCU, opts, result.totalSeconds);
		}
		else {
			Console::SetColorRGB(Console::Theme::GreenR, Console::Theme::GreenG, Console::Theme::GreenB);
			std::cout << "\n  " << Console::Sym::Check << " No CU events.\n";
			Console::Reset();
		}

		// ── Combined C1/C2/CU Overview ──────────────────────
		// Normalize each metric independently so C2/CU are visible
		// alongside much-larger C1 values.
		std::vector<int> bucketC1 = Console::BucketData(c1Vals, GRAPH_WIDTH);
		std::vector<int> bucketC2 = Console::BucketData(c2Vals, GRAPH_WIDTH);
		std::vector<int> bucketCU = Console::BucketData(cuVals, GRAPH_WIDTH);

		int normMaxC1 = std::max(1, *std::max_element(bucketC1.begin(), bucketC1.end()));
		int normMaxC2 = std::max(1, *std::max_element(bucketC2.begin(), bucketC2.end()));
		int normMaxCU = std::max(1, *std::max_element(bucketCU.begin(), bucketCU.end()));

		Console::SetColorRGB(Console::Theme::CyanR, Console::Theme::CyanG, Console::Theme::CyanB);
		std::cout << "\n";
		std::cout << Console::Sym::TopLeft;
		for (int i = 0; i < GRAPH_WIDTH + 8; i++) std::cout << Console::Sym::Horizontal;
		std::cout << Console::Sym::TopRight << "\n";
		Console::SetColorRGB(Console::Theme::WhiteR, Console::Theme::WhiteG, Console::Theme::WhiteB);
		std::cout << "\033[1m  Combined C1/C2/CU Overview\033[22m\n";
		Console::SetColorRGB(Console::Theme::DimR, Console::Theme::DimG, Console::Theme::DimB);
		std::cout << "  Normalized per-metric. ";
		Console::SetColorRGB(Console::Theme::GreenR, Console::Theme::GreenG, Console::Theme::GreenB);
		std::cout << Console::Sym::Bar8 << " C1  ";
		Console::SetColorRGB(Console::Theme::YellowR, Console::Theme::YellowG, Console::Theme::YellowB);
		std::cout << Console::Sym::Bar8 << " C2  ";
		Console::SetColorRGB(Console::Theme::RedR, Console::Theme::RedG, Console::Theme::RedB);
		std::cout << Console::Sym::Bar8 << " CU";
		Console::Reset();
		std::cout << "\n\n";

		constexpr int COMBINED_HEIGHT = 8;
		int labelW = 5;

		for (int row = COMBINED_HEIGHT; row >= 1; row--) {
			double rowFrac = static_cast<double>(row) / COMBINED_HEIGHT;

			Console::SetColorRGB(Console::Theme::DimR, Console::Theme::DimG, Console::Theme::DimB);
			if (row == COMBINED_HEIGHT) std::cout << std::setw(labelW) << "100%";
			else if (row == (COMBINED_HEIGHT + 1) / 2) std::cout << std::setw(labelW) << "50%";
			else if (row == 1) std::cout << std::setw(labelW) << "0%";
			else std::cout << std::string(labelW, ' ');

			std::cout << " " << Console::Sym::Vertical;

			for (int col = 0; col < GRAPH_WIDTH; col++) {
				double normCU = static_cast<double>(bucketCU[col]) / normMaxCU;
				double normC2d = static_cast<double>(bucketC2[col]) / normMaxC2;
				double normC1d = static_cast<double>(bucketC1[col]) / normMaxC1;

				if (normCU >= rowFrac) {
					Console::SetColorRGB(Console::Theme::RedR, Console::Theme::RedG, Console::Theme::RedB);
					std::cout << Console::Sym::Bar8;
				}
				else if (normC2d >= rowFrac) {
					Console::SetColorRGB(Console::Theme::YellowR, Console::Theme::YellowG, Console::Theme::YellowB);
					std::cout << Console::Sym::Bar8;
				}
				else if (normC1d >= rowFrac) {
					Console::SetColorRGB(Console::Theme::GreenR, Console::Theme::GreenG, Console::Theme::GreenB);
					std::cout << Console::Sym::Bar8;
				}
				else {
					std::cout << " ";
				}
			}
			Console::Reset();
			std::cout << "\n";
		}

		Console::SetColorRGB(Console::Theme::DimR, Console::Theme::DimG, Console::Theme::DimB);
		std::cout << std::string(labelW, ' ') << " " << Console::Sym::BottomLeft;
		for (int i = 0; i < GRAPH_WIDTH; i++) std::cout << Console::Sym::Horizontal;
		std::cout << Console::Sym::BottomRight << "\n";

		int padding = labelW + 2;
		int endMin = result.totalSeconds / 60;
		int endSec = result.totalSeconds % 60;
		char endStr[16];
		snprintf(endStr, sizeof(endStr), "%d:%02d", endMin, endSec);
		std::cout << std::string(padding, ' ') << "0:00";
		int gap = GRAPH_WIDTH - 4 - static_cast<int>(strlen(endStr));
		if (gap > 0) std::cout << std::string(gap, ' ');
		std::cout << endStr << "\n";

		Console::SetColorRGB(Console::Theme::DimR, Console::Theme::DimG, Console::Theme::DimB);
		std::cout << std::string(padding, ' ')
			<< "C1 (peak " << result.maxC1PerSecond
			<< ")  C2 (peak " << result.maxC2PerSecond
			<< ")  CU (peak " << result.maxCUPerSecond << ")\n";
		Console::Reset();
	}

	// ── Overall Quality ──────────────────────────────────────
	Console::SetColorRGB(Console::Theme::DimR, Console::Theme::DimG, Console::Theme::DimB);
	std::cout << "\n" << std::string(60, '-') << "\n";

	// ── Primary quality rating ───────────────────────────────
	std::string qr = result.qualityRating;
	if (qr == "EXCELLENT" || qr == "GOOD")
		Console::SetColorRGB(Console::Theme::GreenR, Console::Theme::GreenG, Console::Theme::GreenB);
	else if (qr == "FAIR")
		Console::SetColorRGB(Console::Theme::YellowR, Console::Theme::YellowG, Console::Theme::YellowB);
	else
		Console::SetColorRGB(Console::Theme::RedR, Console::Theme::RedG, Console::Theme::RedB);

	std::cout << "  QUALITY:        " << qr << "\n";
	Console::Reset();

	// ── Total C1 quality tier ────────────────────────────────
	std::cout << "  Total C1:       ";
	if (result.totalC1Quality == "Exceptional" || result.totalC1Quality == "Very good")
		Console::SetColorRGB(Console::Theme::GreenR, Console::Theme::GreenG, Console::Theme::GreenB);
	else if (result.totalC1Quality == "Normal" || result.totalC1Quality == "Marginal")
		Console::SetColorRGB(Console::Theme::YellowR, Console::Theme::YellowG, Console::Theme::YellowB);
	else
		Console::SetColorRGB(Console::Theme::RedR, Console::Theme::RedG, Console::Theme::RedB);
	std::cout << result.totalC1Quality;
	Console::Reset();
	std::cout << " (" << result.totalC1 << " total)\n";

	// ── Archival peak C1 tier ────────────────────────────────
	std::cout << "  Archival Peak:  ";
	if (result.archivalC1Rating == "Ideal" || result.archivalC1Rating == "Good")
		Console::SetColorRGB(Console::Theme::GreenR, Console::Theme::GreenG, Console::Theme::GreenB);
	else if (result.archivalC1Rating == "Acceptable")
		Console::SetColorRGB(Console::Theme::YellowR, Console::Theme::YellowG, Console::Theme::YellowB);
	else
		Console::SetColorRGB(Console::Theme::RedR, Console::Theme::RedG, Console::Theme::RedB);
	std::cout << result.archivalC1Rating;
	Console::Reset();
	std::cout << " (peak " << result.maxC1PerSecond << "/sec)\n";

	Console::SetColorRGB(Console::Theme::DimR, Console::Theme::DimG, Console::Theme::DimB);
	std::cout << std::string(60, '-') << "\n";
	Console::Reset();

	if (qr == "EXCELLENT")
		std::cout << "  Disc is in excellent condition.\n";
	else if (qr == "GOOD")
		std::cout << "  Normal wear, no concerns.\n";
	else if (qr == "FAIR")
		std::cout << "  Elevated C1 rate. Consider cleaning the disc.\n";
	else if (qr == "POOR")
		std::cout << "  Significant errors. Back up this disc.\n";
	else
		std::cout << "  Critical errors detected. Data loss likely.\n";
	std::cout << std::string(60, '=') << "\n";
}

// ============================================================================
// Save Q-Check scan results to CSV
// ============================================================================
bool AudioCDCopier::SaveQCheckLog(const QCheckResult& result, const std::wstring& filename) {
	std::ofstream log(filename);
	if (!log) return false;

	// --- Summary section ---
	log << "# ==============================\n";
	log << "# Plextor Q-Check Quality Scan Log\n";
	log << "# ==============================\n";
	log << "#\n";
	log << "# Quality Rating:        " << result.qualityRating << "\n";
	log << "# Total Sectors:         " << result.totalSectors << "\n";
	log << "# Samples Collected:     " << result.samples.size() << "\n";
	log << "# Disc Length:           "
		<< (result.totalSeconds / 60) << ":"
		<< std::setfill('0') << std::setw(2) << (result.totalSeconds % 60)
		<< std::setfill(' ') << " (mm:ss)\n";
	log << "#\n";
	log << "# --- C1 Statistics ---\n";
	log << "# Total C1:              " << result.totalC1 << "\n";
	log << "# Avg C1/sec:            " << std::fixed << std::setprecision(2)
		<< result.avgC1PerSecond << "\n";
	log << "# Max C1/sec:            " << result.maxC1PerSecond;
	if (result.maxC1SecondIndex >= 0 && result.maxC1SecondIndex < static_cast<int>(result.samples.size()))
		log << " (at LBA " << result.samples[result.maxC1SecondIndex].lba << ")";
	log << "\n";
	log << "# Avg C1/sec Pass:       " << (result.avgC1PerSecond < 220.0 ? "PASS" : "FAIL")
		<< " (Red Book limit: 220/sec)\n";
	log << "#\n";
	log << "# --- C2 Statistics ---\n";
	log << "# Total C2:              " << result.totalC2 << "\n";
	log << "# Avg C2/sec:            " << std::fixed << std::setprecision(2)
		<< result.avgC2PerSecond << "\n";
	log << "# Max C2/sec:            " << result.maxC2PerSecond;
	if (result.maxC2SecondIndex >= 0 && result.maxC2SecondIndex < static_cast<int>(result.samples.size()))
		log << " (at LBA " << result.samples[result.maxC2SecondIndex].lba << ")";
	log << "\n";
	log << "#\n";
	log << "# --- CU Statistics ---\n";
	log << "# Total CU:              " << result.totalCU << "\n";
	log << "# Max CU/sec:            " << result.maxCUPerSecond << "\n";
	log << "#\n";
	log << "# --- Total C1 Quality ---\n";
	log << "# Total C1 Quality:      " << result.totalC1Quality << "\n";
	log << "#\n";
	log << "# --- Archival Audio ---\n";
	log << "# Peak C1 Rating:        " << result.archivalC1Rating << "\n";
	log << "#\n";

	// --- Per-second CSV data ---
	log << "# ==============================\n";
	log << "# Per-Second C1/C2/CU Data\n";
	log << "# ==============================\n";
	log << "Time,Second,LBA,C1,C2,CU\n";

	for (size_t i = 0; i < result.samples.size(); i++) {
		const auto& s = result.samples[i];
		int minutes = static_cast<int>(i) / 60;
		int seconds = static_cast<int>(i) % 60;

		log << minutes << ":" << std::setfill('0') << std::setw(2) << seconds
			<< std::setfill(' ')
			<< "," << i
			<< "," << s.lba
			<< "," << s.c1
			<< "," << s.c2
			<< "," << s.cu << "\n";
	}

	log.close();
	return true;
}