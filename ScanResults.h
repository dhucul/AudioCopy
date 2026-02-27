// ============================================================================
// ScanResults.h - Disc quality scanning results
// ============================================================================
#pragma once

#include "ErrorTypes.h"
#include "AnalysisTypes.h"
#include <windows.h>
#include <vector>
#include <string>
#include <utility>

// ── Per-second sample from Plextor Q-Check hardware scan ────────────────────
struct QCheckSample {
	DWORD lba = 0;          // Approximate LBA at this time slice
	int c1 = 0;             // C1 (BLER) error count for this second
	int c2 = 0;             // C2 (E22) error count for this second
	int cu = 0;             // CU (uncorrectable) count for this second
};

// ── Plextor Q-Check scan result ─────────────────────────────────────────────
// Populated by the hardware-driven quality scan (0xE9/0xEB vendor commands).
// This is the same measurement QPXTool performs — aggregate CIRC decoder
// statistics per time slice, without transferring audio data.
struct QCheckResult {
	bool supported = false;                    // True if drive accepted 0xE9
	DWORD totalSectors = 0;                    // Disc sectors covered
	DWORD totalSeconds = 0;                    // Scan duration in time slices

	// Aggregate C1 statistics
	int totalC1 = 0;
	double avgC1PerSecond = 0.0;
	int maxC1PerSecond = 0;
	int maxC1SecondIndex = -1;

	// Aggregate C2 statistics
	int totalC2 = 0;
	double avgC2PerSecond = 0.0;
	int maxC2PerSecond = 0;
	int maxC2SecondIndex = -1;

	// Aggregate CU statistics
	int totalCU = 0;
	int maxCUPerSecond = 0;

	// Per-second time-series data
	std::vector<QCheckSample> samples;

	// Quality assessment
	std::string qualityRating;                 // EXCELLENT / GOOD / FAIR / POOR / BAD
};

// ── BLER (Block Error Rate) scan result ─────────────────────────────────────
// Captures the output of a detailed error-rate scan.  BLER measures raw error
// frequency before ECC correction.  Red Book spec: < 220 errors/second avg.
struct BlerResult {
	DWORD totalSectors = 0;
	DWORD totalSeconds = 0;
	int totalC2Errors = 0;
	int totalC2Sectors = 0;
	int totalReadFailures = 0;
	int recoveredC2Errors = 0;             // C2 errors the drive corrected internally (sense 0x01)
	int recoveredC2Sectors = 0;            // Sectors with recovered C2 errors
	double avgC2PerSecond = 0.0;
	int maxC2PerSecond = 0;
	DWORD worstSecondLBA = 0;
	int maxC2InSingleSector = 0;
	DWORD worstSectorLBA = 0;
	int consecutiveErrorSectors = 0;
	std::vector<std::pair<DWORD, int>> perSecondC2;
	std::string qualityRating;

	bool hasC1Data = false;
	int totalC1Errors = 0;
	int totalC1Sectors = 0;
	double avgC1PerSecond = 0.0;
	int maxC1PerSecond = 0;
	DWORD worstC1SecondLBA = 0;                // LBA at start of the worst C1 second
	int maxC1InSingleSector = 0;
	DWORD worstC1SectorLBA = 0;
	std::vector<std::pair<DWORD, int>> perSecondC1;

	// Top-N worst individual sectors by C2 error count (LBA, C2 count)
	std::vector<std::pair<DWORD, int>> topWorstC2Sectors;

	DiscZoneStats zoneStats;
	std::vector<ErrorCluster> errorClusters;
	int largestClusterSize = 0;
	bool hasEdgeConcentration = false;
	bool hasProgressivePattern = false;

	// C1 BLER margin / C2 proximity analysis (populated when hasC1Data is true)
	double c1UtilizationPct = -1.0;      // -1 = C1 data unavailable
	double peakC1UtilizationPct = -1.0;  // -1 = C1 data unavailable
	int c2MarginScore = -1;              // 0–100; higher = more headroom; -1 = unavailable
	std::string c2MarginLabel;           // "WIDE", "ADEQUATE", "NARROW", "CRITICAL"
};

// ── Disc rot analysis results ───────────────────────────────────────────────
// Output of the disc rot detection scan.  Combines zone statistics, error
// cluster data, and heuristic indicators for various rot patterns.
struct DiscRotAnalysis {
	DiscZoneStats zones;                        // Error distribution by radial zone
	std::vector<ErrorCluster> clusters;          // Contiguous error regions
	int inconsistentSectors = 0;                // Sectors that read differently on re-read
	int totalRereadTests = 0;                   // Number of re-read comparison tests run
	double inconsistencyRate = 0.0;             // inconsistentSectors / totalRereadTests
	int maxC2InSingleSector = 0;                // Worst C2 count in a single sector

	// Heuristic disc-rot pattern flags
	bool edgeConcentration = false;             // Errors concentrated at inner/outer edges
	bool progressivePattern = false;            // Error rate increases toward the outer edge
	bool pinholePattern = false;                // Many small scattered clusters (pinhole corrosion)
	bool readInstability = false;               // High re-read inconsistency rate

	std::string rotRiskLevel;                   // "NONE", "LOW", "MODERATE", "HIGH", "CRITICAL"
	std::string recommendation;                 // Human-readable advice
};

// ── Comprehensive scan result ───────────────────────────────────────────────
// Aggregates results from every individual scan type into a single report
// with an overall 0–100 score and letter grade.
struct ComprehensiveScanResult {
	BlerResult bler;                                   // Block error rate data
	DiscRotAnalysis rot;                               // Disc rot detection data
	AudioAnalysisResult audio;                         // Audio anomaly data
	std::vector<SpeedComparisonResult> speedComparison;// Speed-dependent error data
	std::vector<MultiPassResult> multiPass;            // Multi-pass consistency data
	std::vector<SeekTimeResult> seekTimes;             // Drive seek latency data

	int overallScore = 0;           // Composite quality score (0–100)
	std::string overallRating;      // Letter grade: A, B, C, D, or F
	std::string summary;            // Human-readable summary paragraph
};

// ── Subchannel burn status result ───────────────────────────────────────────
// Determines whether subchannel data was actually mastered/burned onto the
// disc, or is empty filler.  Useful for deciding if subchannel extraction
// during ripping is worthwhile.
struct SubchannelBurnResult {
	int totalSampled = 0;           // Total sectors sampled
	int readFailures = 0;           // Sectors where ReadSector failed entirely
	int validQCrc = 0;              // Sectors with valid Q-channel CRC-16
	int invalidQCrc = 0;            // Sectors with invalid / absent Q CRC
	int emptySubchannel = 0;        // Sectors where all 96 subchannel bytes are zero
	int rwDataPresent = 0;          // Sectors with non-zero R-W channel data (CD-G, etc.)
	int cdgPacketsFound = 0;        // Sectors with valid CD-G command (0x09) in pack structure
	int validMsfTiming = 0;         // Sectors with correctly incrementing MSF addresses
	int pChannelCorrect = 0;        // Sectors with expected P-channel state (pause/play)
	double qCrcValidPercent = 0.0;  // Percentage of CRC-tested sectors with valid Q CRC
	bool subchannelBurned = false;  // Overall verdict: true = subchannel data is present
	std::string verdict;            // Human-readable summary of the result
	WORD mediaProfile = 0;          // SCSI media profile code (0x0008=CD-ROM, 0x0009=CD-R, etc.)
	std::string mediaTypeName;      // Human-readable media type ("CD-ROM", "CD-R", "CD-RW")
};