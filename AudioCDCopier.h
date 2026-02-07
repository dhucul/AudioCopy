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
	~AudioCDCopier() = default;

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
	int SelectOffset();
	int SelectSecureRipMode();
	SecureRipConfig GetSecureRipConfig(SecureRipMode mode);
	int SelectPregapMode();
	int SelectCacheDefeat();
	int SelectC2Sensitivity();

	// TOC reading
	bool ReadTOC(DiscInfo& disc);
	bool ReadFullTOC(DiscInfo& disc);
	bool ReadCDText(DiscInfo& disc);
	bool ReadISRC(DiscInfo& disc);
	bool DetectHiddenTrack(DiscInfo& disc);

	// Disc reading
	bool ReadDisc(DiscInfo& disc, int errorMode,
		std::function<void(int, int)> progress = nullptr);
	bool ReadDiscSecure(DiscInfo& disc, const SecureRipConfig& config,
		SecureRipResult& result, std::function<void(int, int)> progress = nullptr);
	bool ReadDiscBurst(DiscInfo& disc, std::function<void(int, int)> progress = nullptr);

	// Quality scanning
	bool ScanDiscForC2Errors(const DiscInfo& disc, int scanSpeed = 4, int sensitivity = 2);
	bool RunBlerScan(const DiscInfo& disc, BlerResult& result, int scanSpeed = 8);

	// Output
	bool SaveToFile(const DiscInfo& disc, const std::wstring& basePath);
	bool SaveBlerLog(const BlerResult& result, const std::wstring& filename);
	void PrintBlerGraph(const BlerResult& result, int width = 60, int height = 10);
	bool SaveReadLog(const DiscInfo& disc, const std::wstring& filename);
	bool GenerateCueSheet(const DiscInfo& disc, const std::wstring& audioFilePath,
		const std::wstring& cueOutputPath);

	// Offset handling
	int DetectDriveOffset();
	bool DetectDriveOffset(OffsetDetectionResult& result);
	void ApplyOffsetCorrection(DiscInfo& disc);

	// Media control
	bool Eject() { return m_drive.Eject(); }

	// Enhanced disc rot detection
	bool RunDiscRotScan(DiscInfo& disc, DiscRotAnalysis& result, int scanSpeed);
	bool TestReadConsistency(DWORD lba, int passes, int& inconsistentCount);
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

private:
	ScsiDrive m_drive;

	// Internal constants
	static constexpr int MAX_RETRIES = 5;
	static constexpr int RETRY_SPEED_REDUCTION = 4;

	// Internal reading methods
	bool ReadSectorWithRetry(DWORD lba, BYTE* data, int sectorSize, bool isAudio,
		bool includeSubchannel, int& retryCount, bool detectC2, int* c2Errors);
	bool ReadSectorSecure(DWORD lba, BYTE* data, int sectorSize, bool isAudio,
		const SecureRipConfig& config, SecureSectorResult& result);
	bool DefeatDriveCache(DWORD currentLBA, DWORD maxLBA = 0);  // Add parameter with default
	bool FlushDriveCache();
	uint32_t HashSector(const BYTE* data, int size);
	uint32_t CalculateSectorHash(const BYTE* data);

	// C2 scan helpers
	DWORD CalculateTotalAudioSectors(const DiscInfo& disc) const;
	ScsiDrive::C2ReadOptions BuildC2ReadOptions(int sensitivity, bool& useConditionalMultiPass) const;
	bool RunC2ScanPass1(const DiscInfo& disc, const ScsiDrive::C2ReadOptions& c2Opts,
		DWORD totalSectors, std::vector<std::pair<DWORD, int>>& errorSectors,
		std::vector<DWORD>& pass1ErrorLBAs, int& totalC2Errors, DWORD& scannedSectors);
	void RunConditionalC2ReRead(const ScsiDrive::C2ReadOptions& c2Opts, int sensitivity,
		std::vector<std::pair<DWORD, int>>& errorSectors, std::vector<DWORD>& pass1ErrorLBAs,
		int& totalC2Errors);
	void RunDualSpeedValidation(const DiscInfo& disc, const std::vector<std::pair<DWORD, int>>& errorSectors,
		int totalC2Errors, int scanSpeed);
	void PrintC2ScanReport(const DiscInfo& disc, int sensitivity, int scanSpeed,
		const ScsiDrive::C2ReadOptions& c2Opts, bool useConditionalMultiPass,
		const std::vector<std::pair<DWORD, int>>& errorSectors,
		const std::vector<DWORD>& pass1ErrorLBAs, DWORD scannedSectors);

	// Disc rot analysis helpers
	void ClassifyZone(DWORD lba, DWORD firstLBA, DWORD lastLBA, int c2Errors, DiscZoneStats& zones);
	void DetectErrorClusters(const std::vector<DWORD>& errorLBAs, std::vector<ErrorCluster>& clusters);
	std::string AssessRotRisk(const DiscRotAnalysis& result);
	int CalculateOverallScore(const ComprehensiveScanResult& result);

	// Audio analysis helpers
	bool IsSectorSilent(const BYTE* data);
	bool IsSectorClipped(const BYTE* data);

	// Fingerprint helpers
	int CDDBSum(int n);
	std::string Base64Encode(const BYTE* data, size_t length);
	void SHA1Hash(const BYTE* data, size_t length, BYTE* output);
};