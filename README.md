# AudioCopy

A high-quality Windows command-line tool for ripping audio CDs with AccurateRip verification, secure multi-pass reading, and comprehensive error detection.

![Platform](https://img.shields.io/badge/platform-Windows-blue)
![Language](https://img.shields.io/badge/language-C%2B%2B-orange)
![License](https://img.shields.io/badge/license-MIT-green)

## Overview

AudioCopy is designed for audiophiles and archivists who demand bit-perfect audio extraction from CDs. It combines low-level SCSI drive communication with AccurateRip database verification to ensure the highest quality digital copies of your audio CDs.

## Features

### Core Functionality
- **Bit-Perfect Audio Extraction** - Direct SCSI commands for accurate sector-level reading
- **AccurateRip Verification** - Verify rips against the AccurateRip online database
- **CUE Sheet Generation** - Automatic CUE sheet creation for disc images
- **CD-TEXT Support** - Extract album/track titles and artist information

### Advanced Reading Modes
| Mode | Description |
|------|-------------|
| **Burst** | Maximum speed, no verification - for pristine discs |
| **Standard** | Single-pass read with optional C2 error detection |
| **Fast Secure** | 2 passes with sync on mismatch |
| **Secure** | 3 passes, requires 2 matching reads |
| **Paranoid** | Up to 8 passes, requires 3 consecutive matches |

### Error Detection & Handling
- **C2 Error Pointer Detection** - Hardware-level error reporting
- **BLER Scanning** - Block Error Rate analysis with visual graph output
- **Multi-Pass Verification** - Re-read problematic sectors for accuracy
- **Cache Defeat** - Ensures fresh reads from disc, not drive cache

### Drive Support
- **Automatic Offset Detection** - Calibrate using AccurateRip database
- **Drive Offset Database Lookup** - Query known offsets for your drive model
- **Pre-gap Analysis** - Detect and handle track pre-gaps (INDEX 00)
- **Hidden Track Detection** - Find audio in pre-gap of track 1 (HTOA)
- **Variable Speed Control** - Slow down for damaged discs

### Output Options
- **WAV Files** - Per-track or single image output
- **Detailed Logging** - Console, file, or both
- **Read Quality Reports** - Sector-by-sector accuracy information
- **ISRC Extraction** - Capture International Standard Recording Codes

## Requirements

- Windows 10/11 (64-bit recommended)
- Visual Studio 2022 or later (for building from source)
- CD/DVD drive with audio extraction and C2 error reporting support

## Building from Source

1. **Clone the repository:**
2. **Open in Visual Studio:**
- Open `AudioCopy.vcxproj` or the solution file
- Select Release/x64 configuration

3. **Build:**
- Press `Ctrl+Shift+B` or use __Build > Build Solution__

## Usage

The application provides an interactive menu-driven interface:

1. **Insert an audio CD** into your drive
2. **Select your drive** from the detected list
3. **Configure options:**
   - Read speed (1x-52x or max)
   - Secure rip mode
   - C2 error detection
   - Output format and location
4. **Start ripping** and monitor progress
5. **Verify results** against AccurateRip database

## Project Structure

## Technical Details

### Sector Sizes
| Type | Size (bytes) |
|------|-------------|
| Audio Sector | 2352 |
| Subchannel | 96 |
| Raw Sector | 2448 |
| C2 Error Data | 296 |
| Full Sector + C2 | 2744 |

### AccurateRip CRC Algorithm
AudioCopy implements the AccurateRip v1 and v2 checksum algorithms for track verification, calculating disc IDs from TOC data for database lookups.

## Acknowledgements

This project was inspired by and learned from several excellent audio extraction tools:

### [Exact Audio Copy (EAC)](http://exactaudiocopy.de/)
The gold standard for secure audio CD ripping on Windows. EAC pioneered many techniques used in AudioCopy including secure mode reading, AccurateRip integration, and comprehensive error handling. Special thanks to Andre Wiethoff for creating such a thorough reference implementation.

### [dBpoweramp](https://www.dbpoweramp.com/)
Illustra's dBpoweramp CD Ripper demonstrated how to combine ease-of-use with audiophile-grade accuracy. Its AccurateRip implementation and batch processing capabilities provided valuable insight into efficient ripping workflows.

### [ImgBurn](https://www.imgburn.com/)
Lightning UK's ImgBurn provided inspiration for low-level disc access, sector reading modes, and comprehensive logging. Its detailed drive capability detection and SCSI command implementation were invaluable references.

### [AccurateRip](http://www.accuraterip.com/)
Thanks to Illustrate Ltd. for creating and maintaining the AccurateRip database, enabling verification of bit-perfect audio extraction across millions of CDs worldwide.

### Additional Thanks
- The [Hydrogenaudio Forums](https://hydrogenaud.io/) community for extensive documentation on CD audio extraction
- Microsoft for the Windows SCSI and CD-ROM APIs
- The open-source audio preservation community for algorithm documentation

## Contributing

Contributions are welcome! Please feel free to:
- Report bugs via [GitHub Issues](https://github.com/dhucul/AudioCopy/issues)
- Submit feature requests
- Open Pull Requests with improvements

## License

This project is licensed under the MIT License - see the [LICENSE](LICENSE) file for details.

---

**Disclaimer:** This tool is intended for creating personal backups of CDs you legally own. Please respect copyright laws in your jurisdiction.

