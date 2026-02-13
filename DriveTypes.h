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