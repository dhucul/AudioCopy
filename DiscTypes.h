// ============================================================================
// DiscTypes.h - Data structures for CD disc information
// ============================================================================
#pragma once

#include <windows.h>
#include <vector>
#include <string>
#include <tuple>

// Sector sizes in bytes
constexpr int AUDIO_SECTOR_SIZE = 2352;
constexpr int SUBCHANNEL_SIZE = 96;
constexpr int RAW_SECTOR_SIZE = 2448;
constexpr int C2_ERROR_SIZE = 296;
constexpr int SECTOR_WITH_C2_SIZE = 2648;
constexpr int FULL_SECTOR_WITH_C2 = 2744;

// Drive speed constants
constexpr WORD CD_SPEED_1X = 176;
constexpr WORD CD_SPEED_MAX = 0xFFFF;

// SCSI MMC command opcodes
constexpr BYTE SCSI_READ_CD = 0xBE;
constexpr BYTE SCSI_SET_CD_SPEED = 0xBB;

// Logging output options
enum class LogOutput {
	None = 0,      // No logging
	Console = 1,   // Console output only
	File = 2,      // Log file only
	Both = 3       // Both console and file
};

// Secure rip mode settings
enum class SecureRipMode {
	Burst = -1,      // ← Burst is -1
	Disabled = 0,    // ← Standard single-pass is 0
	Fast = 1,
	Standard = 2,
	Paranoid = 3
};

// Pre-gap extraction mode
enum class PregapMode {
	Include = 0,        // Include pre-gaps in main image (default)
	Skip = 1,           // Don't extract pre-gaps (audio starts at INDEX 01)
	Separate = 2        // Extract pre-gaps as separate files
};

// Secure rip configuration
struct SecureRipConfig {
	SecureRipMode mode = SecureRipMode::Disabled;
	int minPasses = 1;
	int maxPasses = 8;
	int requiredMatches = 2;
	bool useC2 = true;
	bool cacheDefeat = true;
	bool rereadOnC2 = true;
};

// Secure rip log entry for a single sector
struct SecureRipLogEntry {
	DWORD lba = 0;
	int track = 0;
	int phase = 0;             // 1 = first pass, 2 = sweep, 3 = rescue
	int passesUsed = 0;
	int matchCount = 0;
	int c2Errors = 0;
	double readTimeMs = 0.0;
	bool verified = false;
	uint32_t hash = 0;
};

// Per-phase performance summary
struct SecureRipPhaseStats {
	int phase = 0;
	int sectorsProcessed = 0;
	int sectorsVerified = 0;
	int sectorsFailed = 0;
	double durationSeconds = 0.0;
	double avgReadTimeMs = 0.0;
};

// Complete secure rip log
struct SecureRipLog {
	std::string modeName;
	int minPasses = 0;
	int maxPasses = 0;
	int requiredMatches = 0;
	bool useC2 = false;
	bool cacheDefeat = false;

	std::vector<SecureRipLogEntry> entries;
	std::vector<SecureRipPhaseStats> phaseStats;

	int totalSectors = 0;
	int totalVerified = 0;
	int totalUnsecure = 0;
	int totalC2Errors = 0;
	double totalDurationSeconds = 0.0;
};

// Secure rip result for a single sector
struct SecureSectorResult {
	DWORD lba = 0;
	int passesRequired = 0;
	int totalPasses = 0;
	int matchingPasses = 0;
	int c2ErrorPasses = 0;
	bool isSecure = false;
	bool usedCacheDefeat = false;
	uint32_t finalHash = 0;
};

// Summary of secure rip operation
struct SecureRipResult {
	int totalSectors = 0;
	int secureSectors = 0;
	int unsecureSectors = 0;
	int singlePassSectors = 0;
	int multiPassSectors = 0;
	int averagePassesRequired = 0;
	int maxPassesRequired = 0;
	std::vector<SecureSectorResult> problemSectors;
	double securityConfidence = 0.0;
	std::string qualityAssessment;
	SecureRipLog log;  // Detailed log data
};

// TrackInfo: Describes one track on the CD
struct TrackInfo {
	int trackNumber = 0;
	DWORD startLBA = 0;
	DWORD endLBA = 0;
	DWORD pregapLBA = 0;
	bool isAudio = true;
	int mode = 0;
	int session = 1;
	std::string isrc;  // International Standard Recording Code
};

// CDText: Optional metadata stored in the disc's lead-in area
struct CDText {
	std::string albumTitle;
	std::string albumArtist;
	std::vector<std::string> trackTitles;
	std::vector<std::string> trackArtists;
};

// DiscZoneStats: Error distribution across disc zones
struct DiscZoneStats {
	int innerErrors = 0;      // 0-33% of disc
	int middleErrors = 0;     // 33-66% of disc  
	int outerErrors = 0;      // 66-100% of disc
	int innerSectors = 0;
	int middleSectors = 0;
	int outerSectors = 0;

	double InnerErrorRate() const { return innerSectors > 0 ? (double)innerErrors / innerSectors * 100.0 : 0; }
	double MiddleErrorRate() const { return middleSectors > 0 ? (double)middleErrors / middleSectors * 100.0 : 0; }
	double OuterErrorRate() const { return outerSectors > 0 ? (double)outerErrors / outerSectors * 100.0 : 0; }
};

// ErrorCluster: A contiguous region of errors
struct ErrorCluster {
	DWORD startLBA = 0;
	DWORD endLBA = 0;
	int errorCount = 0;
	int size() const { return static_cast<int>(endLBA - startLBA + 1); }
};

// DiscRotAnalysis: Results from disc rot detection scan
struct DiscRotAnalysis {
	DiscZoneStats zones;
	std::vector<ErrorCluster> clusters;
	int inconsistentSectors = 0;        // Sectors that read differently on re-read
	int totalRereadTests = 0;
	double inconsistencyRate = 0.0;

	// Disc rot indicators
	bool edgeConcentration = false;     // Errors concentrated at edges
	bool progressivePattern = false;    // Errors increase toward edge
	bool pinholePattern = false;        // Scattered small clusters
	bool readInstability = false;       // High inconsistent read rate

	std::string rotRiskLevel;           // NONE, LOW, MODERATE, HIGH, CRITICAL
	std::string recommendation;
};

// DiscInfo: Complete state for a disc being copied
struct DiscInfo {
	std::vector<TrackInfo> tracks;
	std::vector<std::vector<BYTE>> rawSectors;
	bool includeSubchannel = true;
	int sessionCount = 1;
	int selectedSession = 0;
	int errorCount = 0;
	std::vector<DWORD> badSectors;
	std::vector<DWORD> c2ErrorSectors;
	bool enableC2Detection = false;
	int totalC2Errors = 0;
	LogOutput loggingOutput = LogOutput::Console;
	std::vector<std::tuple<DWORD, int, double>> readLog;
	uint32_t accurateRipCRC = 0;
	int driveOffset = 0;
	PregapMode pregapMode = PregapMode::Include;
	bool extractHiddenTrack = false;  // Extract hidden track one audio (before track 1 index 1)
	CDText cdText;
	DWORD leadOutLBA = 0;
	bool enableCacheDefeat = false;    // Defeat drive read cache between reads
};

// BlerResult: BLER scan result
struct BlerResult {
	DWORD totalSectors = 0;
	DWORD totalSeconds = 0;
	int totalC2Errors = 0;
	int totalC2Sectors = 0;
	int totalReadFailures = 0;
	double avgC2PerSecond = 0.0;
	int maxC2PerSecond = 0;
	int worstSecondLBA = 0;
	int maxC2InSingleSector = 0;
	DWORD worstSectorLBA = 0;
	int consecutiveErrorSectors = 0;
	std::vector<std::pair<int, int>> perSecondC2;
	std::string qualityRating;

	// Zone stats for BLER (optional use)
	DiscZoneStats zoneStats;
	std::vector<ErrorCluster> errorClusters;
	int largestClusterSize = 0;
	bool hasEdgeConcentration = false;
	bool hasProgressivePattern = false;
};

// Speed comparison test result for detecting surface degradation
struct SpeedComparisonResult {
	DWORD lba = 0;
	int lowSpeedC2 = 0;
	int highSpeedC2 = 0;
	bool inconsistent = false;
};

// Multi-pass verification result
struct MultiPassResult {
	DWORD lba = 0;
	int passesMatched = 0;
	int totalPasses = 0;
	bool allMatch = false;
	uint32_t majorityHash = 0;
};

// Seek time analysis result  
struct SeekTimeResult {
	DWORD fromLBA = 0;
	DWORD toLBA = 0;
	double seekTimeMs = 0;
	bool abnormal = false;
};

// Audio analysis result
struct AudioAnalysisResult {
	int silentSectors = 0;          // Sectors with all zeros
	int clippedSectors = 0;         // Sectors with max amplitude
	int lowLevelSectors = 0;        // Suspiciously quiet sectors
	int dcOffsetSectors = 0;        // Sectors with DC bias
	std::vector<DWORD> suspiciousLBAs;
};

// Comprehensive scan result
struct ComprehensiveScanResult {
	BlerResult bler;
	DiscRotAnalysis rot;
	AudioAnalysisResult audio;
	std::vector<SpeedComparisonResult> speedComparison;
	std::vector<MultiPassResult> multiPass;
	std::vector<SeekTimeResult> seekTimes;

	int overallScore = 0;           // 0-100
	std::string overallRating;      // A, B, C, D, F
	std::string summary;
};

// Extended error information for detailed diagnostics
struct SectorError {
	DWORD lba = 0;
	int trackNumber = 0;
	DWORD scsiSenseKey = 0;
	DWORD scsiASC = 0;          // Additional Sense Code
	DWORD scsiASCQ = 0;         // Additional Sense Code Qualifier
	int retryCount = 0;
	std::string errorType;      // "READ_ERROR", "SEEK_ERROR", "MEDIUM_ERROR", etc.
	double readTimeMs = 0;
};

// Enhanced error statistics
struct ErrorStatistics {
	int totalReadErrors = 0;
	int totalSeekErrors = 0;
	int totalMediumErrors = 0;
	int totalTimeoutErrors = 0;
	int totalCRCErrors = 0;
	int recoveredErrors = 0;
	int unrecoverableErrors = 0;
	std::vector<SectorError> detailedErrors;

	// Error rate tracking per 1000 sectors
	double errorRatePer1000 = 0.0;
	int peakErrorBurst = 0;       // Largest consecutive error run
	DWORD peakBurstStartLBA = 0;
	// New: Statistical measures
	double avgErrorsPerSector = 0.0;
	double errorStdDeviation = 0.0;
	double errorDensity = 0.0;  // Errors per MB
	std::vector<int> errorDistribution;  // Histogram of error counts

	bool hasClusteredErrors = false;
	int clusterCount = 0;
	std::vector<ErrorCluster> errorClusters;
};

// Drive health indicators
struct DriveHealthCheck {
	bool driveResponding = false;
	bool mediaPresent = false;
	bool mediaReady = false;
	bool trayOpen = false;
	bool spinningUp = false;
	bool writeProtected = false;
	std::string mediaType;       // "CD-DA", "CD-ROM", "UNKNOWN"
	int firmwareErrors = 0;
};

// CRC verification result
struct CRCVerification {
	uint32_t calculatedCRC = 0;
	uint32_t expectedCRC = 0;
	bool matches = false;
	std::vector<DWORD> mismatchedSectors;
};

// DriveCapabilities: Describes the capabilities of the CD/DVD drive
struct DriveCapabilities {
	// Identification
	std::string vendor;
	std::string model;
	std::string firmware;
	std::string serialNumber;              // From VPD page 0x80

	// Core capabilities
	bool supportsC2ErrorReporting = false;
	bool supportsAccurateStream = false;
	bool supportsCDText = false;
	bool supportsRawRead = false;

	// Advanced features
	bool supportsOverreadLeadIn = false;
	bool supportsOverreadLeadOut = false;
	bool supportsSubchannelRaw = false;
	bool supportsSubchannelQ = false;
	bool supportsCDDA = false;
	bool supportsMultiSession = false;

	// Audio playback features
	bool supportsDigitalAudioPlay = false;  // DAP - hardware playback
	bool supportsCompositeOutput = false;
	bool supportsSeparateVolume = false;    // Per-channel volume control
	bool supportsSeparateMute = false;

	// Mechanical features
	bool supportsEject = false;
	bool supportsLockMedia = false;
	bool isChanger = false;                 // Multi-disc changer
	int loadingMechanism = 0;               // 0=caddy, 1=tray, 2=popup, 3=changer, 5=slot

	// Performance info
	int maxReadSpeedKB = 0;                 // KB/sec
	int maxWriteSpeedKB = 0;                // KB/sec (0 if read-only)
	int currentReadSpeedKB = 0;             // Current speed setting
	int currentWriteSpeedKB = 0;            // Current write speed setting
	int bufferSizeKB = 0;                   // Drive buffer/cache size
	std::vector<int> supportedReadSpeeds;   // All supported read speeds in KB/s
	std::vector<int> supportedWriteSpeeds;  // All supported write speeds in KB/s

	// Media types - Read
	bool readsCDR = false;
	bool readsCDRW = false;
	bool readsDVD = false;
	bool readsBD = false;

	// Media types - Write
	bool writesCDR = false;
	bool writesCDRW = false;
	bool writesDVD = false;
	bool writesDVDRAM = false;              // DVD-RAM write support
	bool writesBD = false;                  // Blu-ray write support

	// Write features
	bool supportsTestWrite = false;         // Simulation/test mode
	bool supportsBufferUnderrunProtection = false;  // BUP / Burn-Free
	bool supportsWriteTAO = false;          // Track-At-Once
	bool supportsWriteSAO = false;          // Session-At-Once / Disc-At-Once
	bool supportsWriteRAW = false;          // Raw write mode

	// Current media info
	bool mediaPresent = false;
	std::string currentMediaType;
};

// ============================================================================
// Disc Fingerprinting Structures
// ============================================================================

// CDDB/FreeDB style disc ID
struct CDDBFingerprint {
	uint32_t discId = 0;           // 8 hex digit ID (e.g., 0x12045678)
	std::string discIdHex;          // String representation "12045678"
	int trackCount = 0;
	int totalSeconds = 0;
	std::vector<int> trackOffsets;  // Frame offsets for each track
};

// MusicBrainz disc ID (uses TOC data)
struct MusicBrainzFingerprint {
	std::string discId;             // Base64-encoded SHA-1 hash
	int firstTrack = 1;
	int lastTrack = 0;
	int leadOutOffset = 0;          // In frames (LBA + 150)
	std::vector<int> trackOffsets;  // Frame offsets (LBA + 150)
};

// AccurateRip disc identification
struct AccurateRipFingerprint {
	uint32_t discId1 = 0;           // Sum of track offsets
	uint32_t discId2 = 0;           // Sum of (offset * track number)
	uint32_t cddbDiscId = 0;        // CDDB ID for cross-reference
	int trackCount = 0;
	std::vector<uint32_t> trackCRCs; // CRC for each track (if calculated)
};

// Simple audio-based fingerprint (content hash)
struct AudioFingerprint {
	uint32_t audioHash = 0;         // FNV-1a hash of sampled audio data
	uint32_t silenceProfile = 0;    // Hash of silence pattern
	std::vector<uint32_t> trackHashes; // Per-track audio hashes
	int sampleCount = 0;            // Number of samples used
};

// Combined disc fingerprint containing all identification methods
struct DiscFingerprint {
	CDDBFingerprint cddb;
	MusicBrainzFingerprint musicBrainz;
	AccurateRipFingerprint accurateRip;
	AudioFingerprint audio;

	// Metadata
	std::string tocString;          // Human-readable TOC representation
	bool isValid = false;
	std::string generationTime;     // ISO 8601 timestamp

	// Convenience methods
	std::string GetCDDBUrl() const;
	std::string GetMusicBrainzUrl() const;
	std::string GetAccurateRipUrl() const;
};