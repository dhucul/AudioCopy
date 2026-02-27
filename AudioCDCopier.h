// ============================================================================
// AudioCDCopier.h - Main audio CD copying orchestration
// ============================================================================
#pragma once

#include "DiscTypes.h"
#include "ScsiDrive.h"
#include "Progress.h"
#include "ConsoleColors.h"
#include <functional>
#include <string>

class AudioCDCopier {
public:
	AudioCDCopier() = default;
	~AudioCDCopier() { Close(); }

	// Drive access
	bool Open(wchar_t driveLetter) { return m_drive.Open(driveLetter); }
	void Close() { m_drive.Close(); }

	// Configuration menus
	int SelectSpeed();
	int SelectSubchannel();
	int SelectSession(int sessionCount);
	int SelectErrorHandling();
	LogOutput SelectLogging();
	int SelectC2Detection();
	int SelectScanSpeed();
	int SelectWriteSpeed();
	int SelectOffset();
	int SelectSecureRipMode();
	SecureRipConfig GetSecureRipConfig(SecureRipMode mode);
	int SelectPregapMode();
	int SelectCacheDefeat();

	// TOC reading
	bool ReadTOC(DiscInfo& disc);
	bool ReadFullTOC(DiscInfo& disc);
	bool ReadCDText(DiscInfo& disc);
	bool ReadISRC(DiscInfo& disc);
	bool DetectHiddenTrack(DiscInfo& disc);
	bool DetectHiddenLastTrack(DiscInfo& disc);

	// TOC-less disc scan — builds track list from Q subchannel when TOC is bad
	bool ScanDiscWithoutTOC(DiscInfo& disc, int scanSpeed = 4);

	// Disc reading
	bool ReadDisc(DiscInfo& disc, int errorMode,
		std::function<void(int, int)> progress = nullptr);
	bool ReadDiscSecure(DiscInfo& disc, const SecureRipConfig& config,
		SecureRipResult& result, std::function<void(int, int)> progress = nullptr);
	bool ReadDiscBurst(DiscInfo& disc, std::function<void(int, int)> progress = nullptr);

	// Quality scanning
	bool RunBlerScan(const DiscInfo& disc, BlerResult& result, int scanSpeed = 8);
	bool RunQCheckScan(const DiscInfo& disc, QCheckResult& result);
	void PrintQCheckReport(const QCheckResult& result);
	bool SaveQCheckLog(const QCheckResult& result, const std::wstring& filename);
	bool RunC2Scan(const DiscInfo& disc, BlerResult& result, int scanSpeed = 8);
	void PrintC2ScanReport(const BlerResult& result, const DiscInfo& disc, int scanSpeed);
	void PrintC2Chart(const BlerResult& result, int width = 60, int height = 10);
	void PrintC2SenseCodeChart(const std::vector<C2SectorError>& badSectors, const DiscInfo& disc, const BlerResult& result);

	// Output
	bool SaveToFile(const DiscInfo& disc, const std::wstring& basePath);
	bool SaveBlerLog(const BlerResult& result, const std::wstring& filename);
	void PrintBlerGraph(const BlerResult& result, int width = 60, int height = 10);
	bool SaveReadLog(const DiscInfo& disc, const std::wstring& filename);
	bool SaveSecureRipLog(const SecureRipResult& result, const std::wstring& filename);
	bool GenerateCueSheet(const DiscInfo& disc, const std::wstring& audioFilePath,
		const std::wstring& cueOutputPath);

	// BLER Analysis and Reporting
	void AnalyzeBlerResults(BlerResult& result, const std::vector<DWORD>& errorLBAs, int scanSpeed);
	void PrintBlerReport(const DiscInfo& disc, const BlerResult& result);

	// Disc rot C1 integration
	void AnalyzeC1RotPatterns(const QCheckResult& c1Result,
		DWORD firstLBA, DWORD lastLBA, DiscRotAnalysis& analysis);

	// Offset handling
	int DetectDriveOffset();
	bool DetectDriveOffset(OffsetDetectionResult& result);
	void ApplyOffsetCorrection(DiscInfo& disc);

	// Media control
	bool Eject() { return m_drive.Eject(); }

	// Direct drive access (for protection scanning, etc.)
	ScsiDrive& GetDriveRef() { return m_drive; }

	// Enhanced disc rot detection
	bool RunDiscRotScan(DiscInfo& disc, DiscRotAnalysis& result, int scanSpeed);
	bool TestReadConsistency(DWORD lba, int passes, int& inconsistentCount, int readSpeed = 8);
	void AnalyzeErrorPatterns(const std::vector<DWORD>& errorLBAs, DiscRotAnalysis& result);
	void PrintDiscRotReport(const DiscRotAnalysis& result);
	bool SaveDiscRotLog(const DiscRotAnalysis& result, const std::wstring& filename);

	// Additional disc rot detection
	bool RunSpeedComparisonTest(DiscInfo& disc, std::vector<SpeedComparisonResult>& results);
	bool CheckLeadAreas(DiscInfo& disc, int scanSpeed = 4);
	void GenerateSurfaceMap(DiscInfo& disc, const std::wstring& filename, int scanSpeed = 8);

	// Additional error detection methods
	bool RunMultiPassVerification(DiscInfo& disc, std::vector<MultiPassResult>& results,
		int passes = 3, int scanSpeed = 8);
	bool AnalyzeAudioContent(DiscInfo& disc, AudioAnalysisResult& result, int scanSpeed = 16);
	bool RunSeekTimeAnalysis(DiscInfo& disc, std::vector<SeekTimeResult>& results);
	// Accept a scan speed for subchannel verification (default kept for compatibility)
	bool VerifySubchannelIntegrity(DiscInfo& disc, int& errorCount, int scanSpeed = 8);
	bool RunComprehensiveScan(DiscInfo& disc, ComprehensiveScanResult& result, int speed = 8);
	void PrintComprehensiveReport(const ComprehensiveScanResult& result);
	bool SaveComprehensiveReport(const ComprehensiveScanResult& result, const std::wstring& filename);

	// Subchannel burn verification
	bool VerifySubchannelBurnStatus(DiscInfo& disc, SubchannelBurnResult& result, int scanSpeed = 8);
	void PrintSubchannelBurnReport(const SubchannelBurnResult& result);

	// Enhanced error checking
	bool ValidateDiscStructure(const DiscInfo& disc, std::vector<std::string>& issues);
	bool VerifyWrittenFile(const std::wstring& filename, const DiscInfo& disc,
		std::vector<DWORD>& mismatchedSectors);
	bool CheckDiskSpace(const std::wstring& path, DWORD sectorsNeeded);
	bool RunPreflightChecks(DiscInfo& disc, std::vector<std::string>& warnings);
	ErrorStatistics CalculateErrorStatistics(const DiscInfo& disc);

	// CRC verification
	uint32_t CalculateTrackCRC(const DiscInfo& disc, int trackIndex);
	bool VerifyTrackCRCs(const DiscInfo& disc, std::vector<CRCVerification>& results);

	// Drive capabilities
	bool DetectDriveCapabilities(DriveCapabilities& caps);
	void PrintDriveCapabilities(const DriveCapabilities& caps);

	// Disc fingerprinting
	bool GenerateDiscFingerprint(const DiscInfo& disc, DiscFingerprint& fingerprint);
	bool CalculateCDDBId(const DiscInfo& disc, CDDBFingerprint& cddb);
	bool CalculateMusicBrainzId(const DiscInfo& disc, MusicBrainzFingerprint& mb);
	bool CalculateAccurateRipId(const DiscInfo& disc, AccurateRipFingerprint& ar);
	bool CalculateAudioFingerprint(const DiscInfo& disc, AudioFingerprint& audio);
	void PrintDiscFingerprint(const DiscFingerprint& fingerprint);
	bool SaveDiscFingerprint(const DiscFingerprint& fingerprint, const std::wstring& filename);

	// C2 validation
	bool ValidateC2Accuracy(DWORD testLBA);

	// Display drive-specific recommendations
	void ShowDriveRecommendations();
	std::string GetDriveRecommendation() {
		return m_drive.GetDriveRecommendationText();
	}

	// Chipset / controller identification
	bool DetectChipset(ChipsetInfo& info);
	void PrintChipsetInfo(const ChipsetInfo& info);

	// ── Disk Writing Operations ─────────────────────────────────────
	bool WriteDisc(const std::wstring& binFile,
		const std::wstring& cueFile, const std::wstring& subFile,
		int speed, bool usePowerCalibration);

	bool CheckRewritableDisk(bool& isFull, bool& isRewritable);

	bool BlankRewritableDisk(int speed, bool quickBlank = true);

	bool PerformPowerCalibration();

	bool VerifyWriteCompletion(const std::wstring& binFile);

	// ── Production Write Implementation ───────────────────────────
	struct TrackWriteInfo {
		int trackNumber;
		DWORD startLBA;
		DWORD endLBA;
		DWORD pregapLBA;
		bool isAudio;
		bool hasPregap;        // true only when INDEX 00 was explicitly in the CUE file
		std::string isrcCode;
		std::string title;     // CD-Text: track title from CUE TITLE command
		std::string performer; // CD-Text: track performer from CUE PERFORMER command
	};

	bool ParseCueSheet(const std::wstring& cueFile,
		std::vector<TrackWriteInfo>& tracks);
	// Overload that also extracts disc-level CD-Text metadata
	bool ParseCueSheet(const std::wstring& cueFile,
		std::vector<TrackWriteInfo>& tracks,
		std::string& discTitle, std::string& discPerformer);

	bool WriteAudioSectors(const std::wstring& binFile,
		const std::wstring& subFile,
		const std::vector<TrackWriteInfo>& tracks,
		DWORD totalSectors,
		bool hasSubchannel,
		bool needsDeinterleave = false);

	bool VerifyWrittenDisc(const std::vector<TrackWriteInfo>& tracks);

	// Disc balance / wobble detection
	bool CheckDiscBalance(DiscInfo& disc, int& balanceScore);

private:
	ScsiDrive m_drive;
	bool m_hasAccurateStream = false;            // Cached from DetectDriveCapabilities
	bool m_capabilitiesDetected = false;         // Whether capabilities have been queried

	// Ensure drive capabilities have been queried at least once
	void EnsureCapabilitiesDetected();

	// Internal constants
	static constexpr int MAX_RETRIES = 5;
	static constexpr int RETRY_SPEED_REDUCTION = 4;

	// Internal reading methods
	bool ReadSectorWithRetry(DWORD lba, BYTE* data, int sectorSize, bool isAudio,
		bool includeSubchannel, int& retryCount, bool detectC2, int* c2Errors);
	bool ReadSectorSecure(DWORD lba, BYTE* data, int sectorSize, bool isAudio,
		const SecureRipConfig& config, SecureSectorResult& result);
	bool DefeatDriveCache(DWORD currentLBA, DWORD maxLBA = 0);
	bool FlushDriveCache();
	uint32_t HashSector(const BYTE* data, int size);
	uint32_t CalculateSectorHash(const BYTE* data);

	// Shared utility
	DWORD CalculateTotalAudioSectors(const DiscInfo& disc) const;

	// Disc rot analysis helpers
	void ClassifyZone(DWORD lba, DWORD firstLBA, DWORD lastLBA, int c2Errors, DiscZoneStats& zones);
	int CalculateClusterTolerance(int scanSpeed);
	void DetectErrorClusters(const std::vector<DWORD>& errorLBAs, std::vector<ErrorCluster>& clusters, int scanSpeed = 8);
	std::string AssessRotRisk(const DiscRotAnalysis& result);
	int CalculateOverallScore(const ComprehensiveScanResult& result);

	// Audio analysis helpers
	bool IsSectorSilent(const BYTE* data);
	bool IsSectorClipped(const BYTE* data);

	// Fingerprint helpers
	int CDDBSum(int n);
	std::string Base64Encode(const BYTE* data, size_t length);
	void SHA1Hash(const BYTE* data, size_t length, BYTE* output);

	// BLER Report Helpers
	void PrintBlerZoneStats(const BlerResult& result);
	void PrintBlerClusters(const BlerResult& result);
	void PrintBlerWorstSectors(const BlerResult& result);
	void PrintBlerDensityDistribution(const BlerResult& result);
	void PrintBlerPerTrackSummary(const DiscInfo& disc, const BlerResult& result);
	void PrintBlerMarginAnalysis(const BlerResult& result);
	void PrintBlerQualitySummary(const BlerResult& result);
};