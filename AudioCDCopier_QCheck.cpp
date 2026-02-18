#define NOMINMAX
#include "AudioCDCopier.h"
#include "InterruptHandler.h"
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

bool AudioCDCopier::RunQCheckScan(const DiscInfo& disc, QCheckResult& result) {
	std::cout << "\n=== Plextor Q-Check Quality Scan ===\n";

	// ── Verify drive support ─────────────────────────────────
	if (!m_drive.SupportsQCheck()) {
		std::cout << "ERROR: Q-Check commands (0xE9/0xEB) not supported by this drive.\n";
		std::cout << "       This feature requires a Plextor (or compatible Lite-On) drive.\n";
		result.supported = false;
		return false;
	}
	result.supported = true;

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

	std::cout << "Scan range: LBA " << firstLBA << " – " << lastLBA
		<< " (" << result.totalSectors << " sectors, ~"
		<< result.totalSeconds << " sec)\n";
	std::cout << "Drive will scan at ~1x internally (hardware-driven).\n";
	std::cout << "  (Press ESC or Ctrl+C to cancel)\n\n";

	// ── Start the hardware scan ──────────────────────────────
	if (!m_drive.PlextorQCheckStart(firstLBA, lastLBA)) {
		std::cout << "ERROR: Failed to start Q-Check scan (0xE9 rejected).\n";
		return false;
	}

	// ── Poll for results ─────────────────────────────────────
	// The drive scans at ~1x (75 sectors/sec).  We poll every ~500ms to
	// avoid hammering the bus while still providing responsive progress.
	bool scanDone = false;
	int sampleIndex = 0;
	DWORD lastReportedLBA = DWORD(-1);  // FIX: track last LBA to deduplicate

	while (!scanDone) {
		if (InterruptHandler::Instance().IsInterrupted() || InterruptHandler::Instance().CheckEscapeKey()) {
			m_drive.PlextorQCheckStop();
			std::cout << "\n*** Q-Check scan cancelled by user ***\n";
			return false;
		}

		std::this_thread::sleep_for(std::chrono::milliseconds(500));

		int c1 = 0, c2 = 0, cu = 0;
		DWORD currentLBA = 0;

		if (!m_drive.PlextorQCheckPoll(c1, c2, cu, currentLBA, scanDone)) {
			// Transient failure — retry once before giving up
			std::this_thread::sleep_for(std::chrono::milliseconds(200));
			if (!m_drive.PlextorQCheckPoll(c1, c2, cu, currentLBA, scanDone)) {
				m_drive.PlextorQCheckStop();
				std::cout << "\nERROR: Lost communication with drive during Q-Check scan.\n";
				return false;
			}
		}

		// Skip empty samples (drive hasn't started reporting yet)
		if (currentLBA == 0 && c1 == 0 && c2 == 0 && cu == 0 && !scanDone)
			continue;

		// FIX: Deduplicate — polling at 500ms can return the same time slice
		// twice since the drive only advances at ~1 sample/sec.  Without this
		// check, totals and averages are inflated by duplicate entries.
		if (currentLBA == lastReportedLBA && !scanDone)
			continue;
		lastReportedLBA = currentLBA;

		QCheckSample sample;
		sample.lba = currentLBA;
		sample.c1 = c1;
		sample.c2 = c2;
		sample.cu = cu;
		result.samples.push_back(sample);

		// Accumulate totals
		result.totalC1 += c1;
		result.totalC2 += c2;
		result.totalCU += cu;

		if (c1 > result.maxC1PerSecond) {
			result.maxC1PerSecond = c1;
			result.maxC1SecondIndex = sampleIndex;
		}
		if (c2 > result.maxC2PerSecond) {
			result.maxC2PerSecond = c2;
			result.maxC2SecondIndex = sampleIndex;
		}
		if (cu > result.maxCUPerSecond)
			result.maxCUPerSecond = cu;

		sampleIndex++;

		// FIX: Clamp progress to avoid underflow when currentLBA < firstLBA
		// (can happen briefly during drive spin-up).
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
		constexpr int GRAPH_HEIGHT = 10;

		// ── C1 Error Distribution ──
		bool hasC1Data = false;
		for (const auto& s : result.samples) {
			if (s.c1 > 0) { hasC1Data = true; break; }
		}

		if (hasC1Data) {
			PrintQCheckGraph(
				"C1 Error Distribution (BLER)",
				"Each column = a time slice of the disc; height = C1 errors/sec",
				result.samples,
				[](const QCheckSample& s) { return s.c1; },
				result.maxC1PerSecond, result.totalSeconds,
				220,   // Red Book BLER limit
				GRAPH_WIDTH, GRAPH_HEIGHT);
		}

		// ── C2 Error Distribution ──
		bool hasC2Data = false;
		for (const auto& s : result.samples) {
			if (s.c2 > 0) { hasC2Data = true; break; }
		}

		if (hasC2Data) {
			PrintQCheckGraph(
				"C2 Error Distribution",
				"Each column = a time slice of the disc; height = C2 errors/sec",
				result.samples,
				[](const QCheckSample& s) { return s.c2; },
				result.maxC2PerSecond, result.totalSeconds,
				0,     // No Red Book limit for C2 (any C2 is bad)
				GRAPH_WIDTH, GRAPH_HEIGHT);
		}
		else {
			std::cout << "\n=== C2 Error Distribution ===\n";
			std::cout << "  No C2 errors — graph skipped.\n";
		}

		// ── CU Error Distribution ──
		bool hasCUData = false;
		for (const auto& s : result.samples) {
			if (s.cu > 0) { hasCUData = true; break; }
		}

		if (hasCUData) {
			PrintQCheckGraph(
				"CU (Uncorrectable) Distribution",
				"Each column = a time slice of the disc; height = CU events/sec",
				result.samples,
				[](const QCheckSample& s) { return s.cu; },
				result.maxCUPerSecond, result.totalSeconds,
				0,     // No limit — any CU means data loss
				GRAPH_WIDTH, GRAPH_HEIGHT);
		}
		else {
			std::cout << "\n=== CU (Uncorrectable) Distribution ===\n";
			std::cout << "  No CU events — graph skipped.\n";
		}

		// ── Combined C1/C2/CU Summary Chart ─────────────────
		// FIX: Normalize each metric independently to 0.0–1.0 before
		// overlaying.  C1 values are typically 100–1000x larger than C2/CU,
		// so a single shared Y-axis makes C2/CU invisible.  By normalizing
		// per-metric, the chart shows *where* each error type occurs across
		// the disc surface regardless of absolute magnitude.
		std::cout << "\n=== Combined C1/C2/CU Overview ===\n";
		std::cout << "  Normalized view — each metric scaled to its own peak.\n";
		std::cout << "  Legend:  . = C1 only   + = C2 present   ! = CU present\n\n";

		// Bucket into columns (peak per bucket, per metric)
		std::vector<int> bucketC1(GRAPH_WIDTH, 0);
		std::vector<int> bucketC2(GRAPH_WIDTH, 0);
		std::vector<int> bucketCU(GRAPH_WIDTH, 0);
		size_t dataSize = result.samples.size();
		for (size_t i = 0; i < dataSize; i++) {
			size_t col = (i * static_cast<size_t>(GRAPH_WIDTH)) / dataSize;
			if (col >= static_cast<size_t>(GRAPH_WIDTH)) col = static_cast<size_t>(GRAPH_WIDTH) - 1;
			bucketC1[col] = std::max(bucketC1[col], result.samples[i].c1);
			bucketC2[col] = std::max(bucketC2[col], result.samples[i].c2);
			bucketCU[col] = std::max(bucketCU[col], result.samples[i].cu);
		}

		// Per-metric maximums for independent normalization
		int maxC1 = 1, maxC2 = 1, maxCU = 1;
		for (int col = 0; col < GRAPH_WIDTH; col++) {
			if (bucketC1[col] > maxC1) maxC1 = bucketC1[col];
			if (bucketC2[col] > maxC2) maxC2 = bucketC2[col];
			if (bucketCU[col] > maxCU) maxCU = bucketCU[col];
		}

		constexpr int COMBINED_HEIGHT = 8;

		for (int row = COMBINED_HEIGHT; row > 0; row--) {
			// Normalized threshold: fraction of height this row represents
			double rowFrac = static_cast<double>(row) / COMBINED_HEIGHT;

			if (row == COMBINED_HEIGHT)
				std::cout << " 100% |";
			else if (row == (COMBINED_HEIGHT + 1) / 2)
				std::cout << "  50% |";
			else if (row == 1)
				std::cout << "   0% |";
			else
				std::cout << "      |";

			for (int col = 0; col < GRAPH_WIDTH; col++) {
				// Normalize each metric to 0.0–1.0 against its own peak
				double normCU = static_cast<double>(bucketCU[col]) / maxCU;
				double normC2 = static_cast<double>(bucketC2[col]) / maxC2;
				double normC1 = static_cast<double>(bucketC1[col]) / maxC1;

				// Priority: CU > C2 > C1 — show the most severe glyph
				if (normCU >= rowFrac)
					std::cout << '!';
				else if (normC2 >= rowFrac)
					std::cout << '+';
				else if (normC1 >= rowFrac)
					std::cout << '.';
				else
					std::cout << ' ';
			}
			std::cout << "\n";
		}

		std::cout << "      +" << std::string(GRAPH_WIDTH, '-') << "\n";

		int padding = 7;
		int endMin = result.totalSeconds / 60;
		int endSec = result.totalSeconds % 60;
		std::string endStr = std::to_string(endMin) + ":"
			+ (endSec < 10 ? "0" : "") + std::to_string(endSec);

		std::cout << std::string(padding, ' ') << "0:00";
		int gap = GRAPH_WIDTH - 4 - static_cast<int>(endStr.length());
		if (gap > 0) std::cout << std::string(gap, ' ');
		std::cout << endStr << "\n";

		std::cout << std::string(padding, ' ')
			<< ". = C1 (peak " << maxC1 << ")  "
			<< "+ = C2 (peak " << maxC2 << ")  "
			<< "! = CU (peak " << maxCU << ")\n";
	}

	// ── Final verdict ────────────────────────────────────────
	std::cout << "\n" << std::string(60, '-') << "\n";
	std::cout << "  QUALITY: " << result.qualityRating << "\n";

	if (result.qualityRating == "EXCELLENT")
		std::cout << "  Disc is in excellent condition. Minimal C1 corrections.\n";
	else if (result.qualityRating == "GOOD")
		std::cout << "  Normal wear. C1 rate is healthy. Safe to rip.\n";
	else if (result.qualityRating == "FAIR")
		std::cout << "  Elevated C1 rate. Consider cleaning the disc.\n";
	else if (result.qualityRating == "POOR")
		std::cout << "  High error rate. Use secure rip mode. Clean or resurface disc.\n";
	else
		std::cout << "  Uncorrectable errors detected. Some data may be lost.\n";

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