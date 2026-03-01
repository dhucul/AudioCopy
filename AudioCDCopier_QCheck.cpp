#define NOMINMAX
#include "AudioCDCopier.h"
#include "InterruptHandler.h"
#include "ConsoleGraph.h"
#include <iostream>
#include <iomanip>
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
			std::cout << "Scan speed: " << scanSpeed << "x (~" << ((result.totalSeconds / std::max(scanSpeed, 1) / 60) + 1) << " min)\n";
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

		// LiteOn: discard the first few samples — the drive is still
		// seeking/spinning up and reports accumulated startup errors.
		// QPXTool does the same; without this the first sample creates
		// a massive spike that dominates the entire graph.
		if (useLiteOn && sampleIndex < 3) {
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

		double pct = 0.0;
		if (currentLBA >= firstLBA && result.totalSectors > 0) {
			pct = static_cast<double>(currentLBA - firstLBA) * 100.0 / result.totalSectors;
			if (pct > 100.0) pct = 100.0;
		}
		std::cout << "\r  Scanning... " << std::fixed << std::setprecision(1)
			<< pct << "%  LBA " << currentLBA
			<< "  C1=" << c1 << " C2=" << c2 << " CU=" << cu
			<< "        " << std::flush;
	}

	std::cout << "\n";

	// ── Compute averages ─────────────────────────────────────
	result.totalSeconds = static_cast<DWORD>(result.samples.size());
	if (result.totalSeconds > 0) {
		result.avgC1PerSecond = static_cast<double>(result.totalC1) / result.totalSeconds;
		result.avgC2PerSecond = static_cast<double>(result.totalC2) / result.totalSeconds;
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

	PrintQCheckReport(result);
	return true;
}

// ── Helper: render a single Q-Check distribution graph ──────────────────────
// Matches the visual style of PrintBlerGraph: severity glyphs (#/+/.), Y-axis
// labels at top/middle/bottom, time axis, legend, and optional Red Book line.
//
// FIX: Changed from raw function pointer to std::function so that stateless
// lambdas are accepted without relying on implicit conversion guarantees.
static void PrintQCheckGraph(const std::string& title, const std::string& subtitle,
	const std::vector<QCheckSample>& samples,
	std::function<int(const QCheckSample&)> valueFunc,
	int peakValue, DWORD totalSeconds,
	int redBookLimit, int width, int height) {

	std::cout << "\n=== " << title << " ===\n";
	std::cout << "  " << subtitle << "\n\n";

	// Find the maximum value across all samples for Y-axis scaling
	int maxVal = 1;
	for (const auto& s : samples) {
		int v = valueFunc(s);
		if (v > maxVal) maxVal = v;
	}

	// Bucket samples into columns (take peak per bucket, same as PrintBlerGraph)
	std::vector<int> buckets(width, 0);
	size_t dataSize = samples.size();
	for (size_t i = 0; i < dataSize; i++) {
		size_t bucket = (i * static_cast<size_t>(width)) / dataSize;
		if (bucket >= static_cast<size_t>(width)) bucket = static_cast<size_t>(width) - 1;
		buckets[bucket] = std::max(buckets[bucket], valueFunc(samples[i]));
	}

	int labelWidth = std::max(4, static_cast<int>(std::to_string(maxVal).length()) + 1);

	// Determine which row the Red Book limit falls on (if applicable)
	int redBookRow = -1;
	if (redBookLimit > 0 && maxVal > 0) {
		redBookRow = (redBookLimit * height) / maxVal;
		if (redBookRow < 1) redBookRow = 1;
		if (redBookRow > height) redBookRow = -1; // off-scale, skip marker
	}

	for (int row = height; row > 0; row--) {
		int threshold = std::max(1, (maxVal * row) / height);

		// FIX: Bottom row (row==1) represents the lowest visible band, not
		// zero.  Label it with the actual threshold value so the Y-axis
		// is accurate instead of showing a misleading "0".
		if (row == height)
			std::cout << std::setw(labelWidth) << maxVal << " |";
		else if (row == redBookRow)
			std::cout << std::setw(labelWidth) << redBookLimit << " |";
		else if (row == (height + 1) / 2 && row != redBookRow)
			std::cout << std::setw(labelWidth) << (maxVal / 2) << " |";
		else if (row == 1)
			std::cout << std::setw(labelWidth) << threshold << " |";
		else
			std::cout << std::string(labelWidth, ' ') << " |";

		for (int col = 0; col < width; col++) {
			if (row == redBookRow && buckets[col] < threshold) {
				// Draw the Red Book reference line through empty cells
				std::cout << '-';
			}
			else if (buckets[col] >= threshold) {
				double severity = static_cast<double>(buckets[col]) / maxVal;
				if (severity > 0.66) std::cout << '#';
				else if (severity > 0.33) std::cout << '+';
				else std::cout << '.';
			}
			else {
				std::cout << ' ';
			}
		}
		std::cout << "\n";
	}

	// X-axis
	std::cout << std::string(labelWidth, ' ') << " +" << std::string(width, '-') << "\n";

	// Time labels
	int padding = labelWidth + 2;
	int endMin = totalSeconds / 60;
	int endSec = totalSeconds % 60;
	std::string endStr = std::to_string(endMin) + ":"
		+ (endSec < 10 ? "0" : "") + std::to_string(endSec);

	std::cout << std::string(padding, ' ') << "0:00";
	int gap = width - 4 - static_cast<int>(endStr.length());
	if (gap > 0) std::cout << std::string(gap, ' ');
	std::cout << endStr << "\n";

	// Legend
	std::cout << std::string(padding, ' ')
		<< "# = high (>66%)  + = moderate (33-66%)  . = low (<33%)\n";

	// Red Book warning
	if (redBookLimit > 0 && maxVal > redBookLimit) {
		std::cout << std::string(padding, ' ')
			<< "!! Peak (" << peakValue << "/sec) exceeds Red Book limit ("
			<< redBookLimit << "/sec)  [--- = limit line]\n";
	}
	else if (redBookLimit > 0 && redBookRow > 0) {
		std::cout << std::string(padding, ' ')
			<< "--- = Red Book limit (" << redBookLimit << "/sec)\n";
	}
}

void AudioCDCopier::PrintQCheckReport(const QCheckResult& result) {
	std::cout << "\n" << std::string(60, '=') << "\n";
	std::cout << "         PLEXTOR Q-CHECK QUALITY SCAN REPORT\n";
	std::cout << std::string(60, '=') << "\n";

	std::cout << "\n--- Scan Info ---\n";
	std::cout << "  Samples collected: " << result.totalSeconds << "\n";
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
	std::cout << "\n";

	if (result.avgC1PerSecond < 5.0)
		std::cout << "  C1 Assessment: EXCELLENT — minimal correction needed\n";
	else if (result.avgC1PerSecond < 50.0)
		std::cout << "  C1 Assessment: GOOD — normal wear\n";
	else if (result.avgC1PerSecond < 220.0)
		std::cout << "  C1 Assessment: FAIR — elevated but within Red Book limits\n";
	else
		std::cout << "  C1 Assessment: POOR — exceeds Red Book BLER limit\n";

	// ── C2 (E22) ─────────────────────────────────────────────
	std::cout << "\n--- C2 Errors ---\n";
	std::cout << "  Total C2:    " << result.totalC2 << "\n";
	std::cout << "  Avg C2/sec:  " << std::fixed << std::setprecision(2)
		<< result.avgC2PerSecond << "\n";
	std::cout << "  Max C2/sec:  " << result.maxC2PerSecond;
	if (result.maxC2SecondIndex >= 0 && result.maxC2SecondIndex < static_cast<int>(result.samples.size()))
		std::cout << "  (at LBA " << result.samples[result.maxC2SecondIndex].lba << ")";
	std::cout << "\n";

	if (result.totalC2 == 0)
		std::cout << "  C2 Assessment: PERFECT — no uncorrectable CIRC errors\n";
	else if (result.avgC2PerSecond < 1.0)
		std::cout << "  C2 Assessment: ACCEPTABLE — few uncorrectable errors\n";
	else
		std::cout << "  C2 Assessment: POOR — significant uncorrectable errors\n";

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

	std::string qr = result.qualityRating;
	if (qr == "EXCELLENT" || qr == "GOOD")
		Console::SetColorRGB(Console::Theme::GreenR, Console::Theme::GreenG, Console::Theme::GreenB);
	else if (qr == "FAIR")
		Console::SetColorRGB(Console::Theme::YellowR, Console::Theme::YellowG, Console::Theme::YellowB);
	else
		Console::SetColorRGB(Console::Theme::RedR, Console::Theme::RedG, Console::Theme::RedB);

	std::cout << "  QUALITY: " << qr << "\n";
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
	log << "# Samples Collected:     " << result.totalSeconds << "\n";
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