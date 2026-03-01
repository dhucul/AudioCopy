#define NOMINMAX
#include "AudioCDCopier.h"
#include "AccurateRip.h"
#include "InterruptHandler.h"
#include "MenuHelpers.h"
#include <iostream>
#include <fstream>
#include <iomanip>


// ============================================================================
// Menu Selection Methods
// ============================================================================

int AudioCDCopier::SelectSpeed() {
	std::cout << "\n=== Speed ===\n0. Back to menu\n1. Max\n2. 1x (best quality)\n3. 4x (recommended)\n4. Custom\nChoice: ";
	int c = GetMenuChoice(0, 4, 3);
	std::cin.clear(); std::cin.ignore(10000, '\n');

	if (c == 0) return -1;
	if (c == 1) { m_drive.SetSpeed(0); std::cout << "WARNING: Max speed - accuracy not guaranteed\n"; return 0; }
	if (c == 2) { m_drive.SetSpeed(1); std::cout << "1x speed\n"; return 1; }
	if (c == 3) { m_drive.SetSpeed(4); std::cout << "4x speed\n"; return 4; }

	std::cout << "Enter multiplier (1-52): ";
	std::string s; std::cin >> s;
	if (!s.empty() && (s.back() == 'x' || s.back() == 'X')) s.pop_back();
	int m = 4;
	try { m = std::stoi(s); }
	catch (...) {}
	if (m < 1) m = 1; if (m > 52) m = 52;
	m_drive.SetSpeed(m);
	if (m >= 8)
		std::cout << "WARNING: " << m << "x speed - accuracy may be reduced\n";
	else
		std::cout << m << "x speed\n";
	std::cin.clear(); std::cin.ignore(10000, '\n');
	return m;
}

int AudioCDCopier::SelectSubchannel() {
	std::cout << "\n=== Subchannel Data ===\n";
	std::cout << "0. Back to menu\n";
	std::cout << "1. Include subchannel (for accurate rip, creates .sub file)\n";
	std::cout << "2. Audio only (smaller output, no .sub file)\n";
	std::cout << "Choice: ";
	int c = GetMenuChoice(0, 2, 1);
	std::cin.clear(); std::cin.ignore(10000, '\n');
	if (c == 0) return -1;
	bool include = (c != 2);
	std::cout << (include ? "Including subchannel data\n" : "Audio only mode\n");
	return include ? 1 : 0;
}

int AudioCDCopier::SelectErrorHandling() {
	std::cout << "\n=== Error Handling ===\n";
	std::cout << "0. Back to menu\n";
	std::cout << "1. Abort on error (safest)\n";
	std::cout << "2. Skip bad sectors (fill with silence)\n";
	std::cout << "3. Skip bad sectors (keep reading)\nChoice: ";
	int c = GetMenuChoice(0, 3, 1);
	std::cin.clear(); std::cin.ignore(10000, '\n');
	if (c == 0) return -1;
	if (c == 2) { std::cout << "Will fill bad sectors with silence\n"; return 2; }
	if (c == 3) { std::cout << "Will skip bad sectors\n"; return 3; }
	std::cout << "Will abort on error\n";
	return 1;
}

LogOutput AudioCDCopier::SelectLogging() {
	std::cout << "\n=== Logging Output ===\n";
	std::cout << "0. Back to menu\n";
	std::cout << "1. Log to file\n";
	std::cout << "2. Log to console\n";
	std::cout << "3. Log to both\n";
	std::cout << "4. No logging\n";
	std::cout << "Choice: ";
	int c = GetMenuChoice(0, 4, 1);
	std::cin.clear(); std::cin.ignore(10000, '\n');
	if (c == 0) return LogOutput::None;  // Sentinel for "back"
	switch (c) {
	case 1: return LogOutput::File;
	case 2: return LogOutput::Console;
	case 3: return LogOutput::Both;
	default: return LogOutput::None;
	}
}

int AudioCDCopier::SelectC2Detection() {
	std::cout << "\n=== C2 Error Detection ===\n";
	std::cout << "C2 errors indicate uncorrectable read errors (data corruption).\n";
	std::cout << "Checking drive C2 support... ";
	if (!m_drive.CheckC2Support()) {
		std::cout << "NOT SUPPORTED\n";
		return 0;
	}
	std::cout << "OK\n";
	std::cout << "0. Back to menu\n";
	std::cout << "1. Enable C2 error detection (recommended)\n";
	std::cout << "2. Disable C2 detection (faster)\nChoice: ";
	int c = GetMenuChoice(0, 2, 1);
	std::cin.clear(); std::cin.ignore(10000, '\n');
	if (c == 0) return -1;
	bool enable = (c == 1);
	std::cout << (enable ? "C2 error detection enabled\n" : "C2 detection disabled\n");
	return enable ? 1 : 0;
}

int AudioCDCopier::SelectOffset() {
	std::cout << "\n=== Drive Offset ===\n0. Back to menu\n1. Auto-detect\n2. Enter manually\n3. No correction\nChoice: ";
	int c = GetMenuChoice(0, 3, 1);
	std::cin.ignore(10000, '\n');
	if (c == 0) return -1;  // ✓ Changed from MENU_BACK to -1 for consistency
	if (c == 1) {
		int off = DetectDriveOffset();
		std::cout << "Using offset: " << off << "\n";
		return off;
	}
	if (c == 2) {
		std::cout << "Enter offset: ";
		int off = 0;
		std::cin >> off;
		std::cin.ignore(10000, '\n');
		return off;
	}
	return 0;
}

int AudioCDCopier::SelectScanSpeed() {
	std::cout << "\n=== Scan Speed ===\n";
	std::cout << "Slower speeds are more accurate for damaged discs.\n";
	std::cout << "NOTE: Speeds 8x and above significantly reduce accuracy.\n\n";
	std::cout << "0. Back to menu\n";
	std::cout << "1. 1x (most accurate)\n";
	std::cout << "2. 2x (recommended)\n";
	std::cout << "3. 4x (good for undamaged discs)\n";
	std::cout << "4. 8x (faster, reduced accuracy)\n";
	std::cout << "5. Max speed (least accurate)\n";
	std::cout << "Choice: ";
	int c = GetMenuChoice(0, 5, 2);
	std::cin.clear(); std::cin.ignore(10000, '\n');
	switch (c) {
	case 0: return -1;
	case 1: std::cout << "Using 1x speed\n"; return 1;
	case 2: std::cout << "Using 2x speed\n"; return 2;
	case 3: std::cout << "Using 4x speed\n"; return 4;
	case 4: std::cout << "WARNING: 8x may reduce rip accuracy\n"; return 8;
	case 5: std::cout << "WARNING: Max speed - accuracy not guaranteed\n"; return 0;
	default: return 2;
	}
}

int AudioCDCopier::SelectSecureRipMode() {
	std::cout << "\n=== Rip Mode ===\n";
	std::cout << "Choose verification level for accuracy vs speed.\n\n";
	std::cout << "0. Back to menu\n";
	std::cout << "1. Standard (single pass with retry on error)\n";
	std::cout << "2. Secure - Fast (2 passes, light verification)\n";
	std::cout << "3. Secure - Standard (3-6 passes, balanced accuracy)\n";
	std::cout << "4. Secure - Paranoid (4-8 passes, maximum accuracy)\n";
	std::cout << "5. Burst (maximum speed, no verification)\n";

	int c = GetMenuChoice(0, 5, 1);
	std::cin.clear(); std::cin.ignore(10000, '\n');

	switch (c) {
	case 0: return -1;
	case 1: std::cout << "Standard mode - Single pass\n"; return 0;
	case 2: std::cout << "Secure Fast mode - Light verification\n"; return 1;
	case 3: std::cout << "Secure Standard mode - Balanced accuracy\n"; return 2;
	case 4: std::cout << "Secure Paranoid mode - Maximum accuracy\n"; return 3;
	case 5: std::cout << "Burst mode - Maximum speed\n"; return -2;
	default: return 0;
	}
}

int AudioCDCopier::SelectPregapMode() {
	std::cout << "\n=== Pre-gap Extraction ===\n";
	std::cout << "Pre-gaps are the silent/hidden sections before each track's INDEX 01.\n";
	std::cout << "Track 1 may contain hidden audio (HTOA) before INDEX 01.\n\n";
	std::cout << "0. Back to menu\n";
	std::cout << "1. Include pre-gaps in image (recommended for accurate backup)\n";
	std::cout << "2. Skip pre-gaps (audio starts at INDEX 01)\n";
	std::cout << "3. Extract pre-gaps as separate files\n";
	std::cout << "Choice: ";

	int c = GetMenuChoice(0, 3, 1);
	std::cin.clear(); std::cin.ignore(10000, '\n');

	if (c == 0) return -1;

	const char* modes[] = { "", "Including pre-gaps in image", "Skipping pre-gaps", "Extracting pre-gaps separately" };
	std::cout << modes[c] << "\n";

	return c - 1;
}

int AudioCDCopier::SelectCacheDefeat() {
	std::cout << "\n=== Drive Cache Defeat ===\n";
	std::cout << "CD drives cache recently read sectors in memory. When re-reading\n";
	std::cout << "the same sector, the drive may return cached data instead of\n";
	std::cout << "actually reading from the disc again.\n\n";
	std::cout << "Cache defeat forces a seek to a distant location between reads,\n";
	std::cout << "ensuring each read comes from the actual disc surface.\n\n";
	std::cout << "  - ENABLED:  More accurate, detects intermittent read errors\n";
	std::cout << "              Slower due to extra seeks (~2-3x slower)\n";
	std::cout << "  - DISABLED: Faster, but may miss marginal/unstable sectors\n\n";
	std::cout << "0. Back to menu\n";
	std::cout << "1. Enable cache defeat (recommended for damaged discs)\n";
	std::cout << "2. Disable cache defeat (faster)\n";
	std::cout << "Choice: ";

	int c = GetMenuChoice(0, 2, 2);
	std::cin.clear(); std::cin.ignore(10000, '\n');

	if (c == 0) return -1;
	bool enable = (c == 1);
	std::cout << (enable ? "Cache defeat ENABLED - seeks between reads\n"
		: "Cache defeat DISABLED - maximum speed\n");
	return enable ? 1 : 0;
}

int AudioCDCopier::SelectSession(int sessionCount) {
	if (sessionCount <= 1) return 0;

	std::cout << "\n=== Session Selection ===\n";
	std::cout << "Disc has " << sessionCount << " sessions.\n";
	std::cout << "0. Back to menu\n";
	for (int i = 1; i <= sessionCount; i++) {
		std::cout << i << ". Session " << i;
		if (i == sessionCount) std::cout << " (latest)";
		std::cout << "\n";
	}
	std::cout << (sessionCount + 1) << ". All sessions\n";
	std::cout << "Choice: ";

	int c = GetMenuChoice(0, sessionCount + 1, sessionCount);
	std::cin.clear(); std::cin.ignore(10000, '\n');
	if (c == 0) return -1;
	if (c == sessionCount + 1) return 0;  // All sessions
	return c;
}

SecureRipConfig AudioCDCopier::GetSecureRipConfig(SecureRipMode mode) {
	SecureRipConfig config;
	config.mode = mode;

	switch (mode) {
	case SecureRipMode::Disabled:
		config.minPasses = 1;
		config.maxPasses = 1;
		config.requiredMatches = 1;
		config.useC2 = true;
		config.c2Guided = true;
		config.cacheDefeat = false;
		config.rereadOnC2 = true;
		config.maxSpeed = 0;  // No limit
		break;
	case SecureRipMode::Fast:
		config.minPasses = 2;
		config.maxPasses = 3;
		config.requiredMatches = 2;
		config.useC2 = true;
		config.c2Guided = true;   // Trust clean C2 reads to skip re-reads
		config.cacheDefeat = true;
		config.rereadOnC2 = true;
		config.maxSpeed = 4;  // Cap at 4x for accuracy
		break;
	case SecureRipMode::Standard:
		config.minPasses = 3;
		config.maxPasses = 6;
		config.requiredMatches = 2;
		config.useC2 = true;
		config.c2Guided = true;
		config.cacheDefeat = true;
		config.rereadOnC2 = true;
		config.maxSpeed = 4;  // Cap at 4x for accuracy
		break;
	case SecureRipMode::Paranoid:
		config.minPasses = 4;
		config.maxPasses = 8;
		config.requiredMatches = 3;
		config.useC2 = true;
		config.c2Guided = false;  // Never skip re-reads, always verify fully
		config.cacheDefeat = true;
		config.rereadOnC2 = true;
		config.maxSpeed = 2;  // Cap at 2x for maximum accuracy
		break;
	}

	return config;
}

// ============================================================================
// Offset Detection
// ============================================================================

int AudioCDCopier::DetectDriveOffset() {
	std::string vendor, model;
	if (m_drive.GetDriveInfo(vendor, model)) {
		std::cout << "Drive: " << vendor << " " << model << "\n";
	}

	// Try the enhanced detection method first
	OffsetDetectionResult result;
	if (m_drive.DetectDriveOffset(result)) {
		std::cout << "Offset detected: " << result.offset << " samples\n";
		std::cout << "Confidence: " << result.confidence << "%\n";
		std::cout << "Method: " << result.details << "\n";
		return result.offset;
	}

	std::cout << "Could not auto-detect offset from disc.\n";
	std::cout << "Recommendation: Use a known test disc or enter offset manually.\n";
	return 0;
}

bool AudioCDCopier::DetectDriveOffset(OffsetDetectionResult& result) {
	return m_drive.DetectDriveOffset(result);
}

int AudioCDCopier::SelectWriteSpeed() {
	std::cout << "\n=== Write Speed ===\n";
	std::cout << "Lower speeds produce more reliable burns.\n\n";
	std::cout << "0. Back to menu\n";
	std::cout << "1. 1x (safest, slowest)\n";
	std::cout << "2. 2x (recommended for audio)\n";
	std::cout << "3. 4x (good balance)\n";
	std::cout << "4. 8x (faster, may reduce quality)\n";
	std::cout << "5. Max speed (fastest, least reliable)\n";
	std::cout << "Choice: ";
	int c = GetMenuChoice(0, 5, 2);
	std::cin.clear(); std::cin.ignore(10000, '\n');
	switch (c) {
	case 0: return -1;
	case 1: std::cout << "Using 1x write speed\n"; return 1;
	case 2: std::cout << "Using 2x write speed\n"; return 2;
	case 3: std::cout << "Using 4x write speed\n"; return 4;
	case 4: std::cout << "WARNING: 8x may reduce burn quality\n"; return 8;
	case 5: std::cout << "WARNING: Max speed - burn quality not guaranteed\n"; return 0;
	default: return 2;
	}
}