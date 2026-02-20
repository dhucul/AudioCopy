// ============================================================================
// DriveTypes.h - Drive capability and health structures
// ============================================================================
#pragma once

#include <windows.h>
#include <vector>
#include <string>

// ── Drive health indicators ─────────────────────────────────────────────────
// Quick status check for the physical drive and inserted media.
struct DriveHealthCheck {
	bool driveResponding = false;  // Drive answers SCSI TEST UNIT READY
	bool mediaPresent = false;     // A disc is inserted
	bool mediaReady = false;       // Disc is spun up and readable
	bool trayOpen = false;         // Drive tray is open
	bool spinningUp = false;       // Drive is in the process of spinning up
	bool writeProtected = false;   // Media is read-only
	std::string mediaType;         // "CD-DA", "CD-ROM", "UNKNOWN"
	int firmwareErrors = 0;        // Firmware-reported error count
};

// ── CD-ROM chipset / controller identification ──────────────────────────────
// Populated by probing SCSI INQUIRY data, vendor strings, firmware signatures,
// and known model-to-chipset mappings.  Useful for understanding drive quirks
// and setting optimal extraction parameters.
enum class ChipsetFamily {
	Unknown,
	MediaTek,        // MediaTek (formerly MT1818/MT1898 etc.) - most modern slim drives
	Renesas,         // Renesas (NEC) - used in many USB/laptop drives
	Panasonic,       // Panasonic/Matsushita MN103 series
	Sanyo,           // Sanyo LC897xx series - older CD/DVD
	Philips,         // Philips SAA78xx / CDD series
	Sony,            // Sony CXD series
	Plextor,         // Plextor custom (Sanyo-derived) - premium audio drives
	LiteOn,          // LiteOn (MediaTek-based) - common desktop drives
	Pioneer,         // Pioneer custom chipsets
	Realtek,         // Realtek USB bridge controllers
	JMicron,         // JMicron JMS578/JMS567 USB-SATA bridges
	ASMedia,         // ASMedia ASM1153/ASM1351 USB bridges
	VIA,             // VIA VT6315 / VT1708 series
	NEC,             // NEC/Renesas legacy controllers
	Ricoh            // Ricoh controllers
};

struct ChipsetInfo {
	ChipsetFamily family = ChipsetFamily::Unknown;
	std::string chipsetName;           // e.g. "MediaTek MT1959"
	std::string detectionMethod;       // How the chipset was identified
	std::string interfaceType;         // "SATA", "USB", "IDE/ATAPI"
	std::string usbBridge;             // USB bridge chip if detected (e.g. "JMicron JMS578")
	bool isUSBAttached = false;        // Drive is behind a USB bridge
	bool knownAudioQuirks = false;     // Chipset has known audio extraction issues
	std::string quirkDescription;      // Description of known quirks
	int confidencePercent = 0;         // 0-100 detection confidence
};

// ── Drive capabilities descriptor ───────────────────────────────────────────
// Populated by querying the drive's SCSI MODE SENSE / GET CONFIGURATION pages.
// Used to determine rip-quality suitability and available features.
struct DriveCapabilities {
	// ── Identification ──
	std::string vendor;                           // e.g. "PLEXTOR"
	std::string model;                            // e.g. "PX-716A"
	std::string firmware;                         // Firmware revision string
	std::string serialNumber;                     // From VPD page 0x80

	// ── Core ripping capabilities ──
	bool supportsC2ErrorReporting = false;        // Can return C2 error pointers
	bool supportsAccurateStream = false;          // Guarantees no jitter in streaming reads
	bool supportsCDText = false;                  // Can read CD-TEXT from lead-in
	bool supportsWriteCDText = false;             // Can write CD-TEXT to lead-in
	bool supportsRawRead = false;                 // Can perform raw sector reads (0xBE)

	// ── Advanced features ──
	bool supportsOverreadLeadIn = false;          // Can read before LBA 0 (lead-in overread)
	bool supportsOverreadLeadOut = false;         // Can read past the lead-out
	bool supportsSubchannelRaw = false;           // Can return raw P–W subchannel data
	bool supportsSubchannelQ = false;             // Can return deinterleaved Q subchannel
	bool supportsCDDA = false;                    // Digital audio extraction supported
	bool supportsMultiSession = false;            // Can read multi-session discs

	// ── Audio playback features ──
	bool supportsDigitalAudioPlay = false;        // Hardware DAP (digital audio playback)
	bool supportsCompositeOutput = false;         // Composite audio output jack
	bool supportsSeparateVolume = false;          // Per-channel volume control
	bool supportsSeparateMute = false;            // Per-channel mute

	// ── Mechanical features ──
	bool supportsEject = false;                   // Can eject the tray via software
	bool supportsLockMedia = false;               // Can lock the tray closed
	bool isChanger = false;                       // Multi-disc changer mechanism
	int loadingMechanism = 0;                     // 0=caddy, 1=tray, 2=popup, 3=changer, 5=slot

	// ── Performance info ──
	int maxReadSpeedKB = 0;                       // Maximum read speed in KB/s
	int maxWriteSpeedKB = 0;                      // Maximum write speed (0 = read-only drive)
	int currentReadSpeedKB = 0;                   // Currently configured read speed
	int currentWriteSpeedKB = 0;                  // Currently configured write speed
	int bufferSizeKB = 0;                         // Drive internal buffer / cache size
	std::vector<int> supportedReadSpeeds;         // All supported read speeds in KB/s
	std::vector<int> supportedWriteSpeeds;        // All supported write speeds in KB/s

	// ── Readable media types ──
	bool readsCDR = false;
	bool readsCDRW = false;
	bool readsDVD = false;
	bool readsBD = false;

	// ── Writable media types ──
	bool writesCDR = false;
	bool writesCDRW = false;
	bool writesDVD = false;
	bool writesDVDRAM = false;                    // DVD-RAM write support
	bool writesBD = false;                        // Blu-ray write support

	// ── Write features ──
	bool supportsTestWrite = false;               // Simulation / test-write mode
	bool supportsBufferUnderrunProtection = false; // BUP / Burn-Free
	bool supportsWriteTAO = false;                // Track-At-Once
	bool supportsWriteSAO = false;                // Session-At-Once / Disc-At-Once
	bool supportsWriteRAW = false;                // Raw write mode

	// ── Current media info ──
	bool mediaPresent = false;                    // Whether a disc is currently loaded
	std::string currentMediaType;                 // Detected media type string
};

// ── Drive-specific configuration overrides ───────────────────────────────────
// Used for drives with known quirks or optimal settings for audio extraction.
struct DriveSpecificConfig {
	std::string vendor;
	std::string model;
	
	// Audio extraction settings
	int readOffset = 0;                           // Sample offset correction
	bool forceAccurateStream = false;             // Override detection
	bool forceC2ErrorReporting = false;           // Enable/disable C2
	bool disableC2ErrorReporting = false;         // Explicitly disable C2
	
	// Speed settings for best audio quality
	int recommendedReadSpeed = 0;                 // 0 = max, otherwise KB/s
	bool limitSpeedForAudio = false;              // True if slower = better quality
	
	// Behavioral quirks
	bool requiresExtraSpinup = false;             // Need longer spin-up time
	bool hasJitterIssues = false;                 // Known to produce jitter
	bool requiresCacheFlush = false;              // Flush cache between reads
	int retryCount = 3;                           // Default retry count for errors
	
	// Feature overrides
	bool supportsOverreadLeadIn = false;          // Override capability detection
	bool supportsOverreadLeadOut = false;
	
	// Burst mode optimization (NEW FIELDS)
	bool optimizedForBurst = false;               // Drive benefits from multi-sector reads
	int burstReadSize = 26;                       // Optimal sectors per batch (0 = default)
	bool cacheBehaviorPredictable = false;        // Cache doesn't interfere with accuracy
	bool fastSeekRecovery = false;                // Quick recovery after cache defeat seeks
};

// Known drive configurations for optimal audio extraction
static const DriveSpecificConfig knownDriveConfigs[] = {
	// ═══════════════════════════════════════════════════════════════
	// Pioneer BD Drives - EXCELLENT for audio extraction
	// ═══════════════════════════════════════════════════════════════
	{
		"PIONEER", "BDR-S13U",
		667,                     // readOffset (AccurateRip verified)
		true,                    // forceAccurateStream (guaranteed jitter-free)
		true,                    // forceC2ErrorReporting (reliable C2)
		false,                   // disableC2ErrorReporting
		10560,                   // recommendedReadSpeed (24x = maximum)
		false,                   // limitSpeedForAudio (excellent at full speed)
		false,                   // requiresExtraSpinup
		false,                   // hasJitterIssues (rock-solid mechanical)
		false,                   // requiresCacheFlush (smart cache design)
		2,                       // retryCount (usually succeeds first try)
		true,                    // supportsOverreadLeadIn
		true,                    // supportsOverreadLeadOut
		true,                    // optimizedForBurst ← KEY FEATURE
		26,                      // burstReadSize (optimal batch size)
		true,                    // cacheBehaviorPredictable ← SAFE CACHE
		true                     // fastSeekRecovery
	},
	{
		"PIONEER", "BD-RW BDR-S12U",
		667, true, true, false, 8467, false, false, false, false, 3, true, true,
		true, 26, true, true  // Burst optimized
	},
	{
		"PIONEER", "BD-RW BDR-209",
		667, true, true, false, 8467, false, false, false, false, 3, true, true,
		true, 26, true, true  // Burst optimized
	},
	
	// Plextor Premium drives - legendary audio quality
	{
		"PLEXTOR", "CD-R PREMIUM2",
		30, true, true, false, 0, false, false, false, false, 2, true, true,
		true, 26, true, false  // Burst capable
	},
	{
		"PLEXTOR", "CD-R PREMIUM",
		30, true, true, false, 0, false, false, false, false, 2, true, true,
		true, 26, true, false  // Burst capable
	},
	
	// LG drives - good but may need slower speeds
	{
		"LG", "BD-RE WH16NS60",
		6, false, false, false, 4233, true, false, false, false, 5, false, false,
		false, 26, false, false  // Single-sector recommended
	}
};