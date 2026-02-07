# AudioCopy

A Windows command-line tool for high-quality audio CD ripping with advanced disc diagnostics, written in C++.

AudioCopy reads audio CDs at the raw sector level using SCSI/MMC commands and provides multiple quality scanning modes to assess disc health before, during, or after extraction.

---

## Features

### Ripping
- **Burst, standard, and secure ripping** with configurable multi-pass verification and cache defeat
- **Drive read offset correction** with auto-detection (AccurateRip database, pregap analysis, or manual)
- **AccurateRip V1** checksum calculation and online verification
- **Pre-gap extraction** (include in image, skip, or extract separately)
- **Subchannel reading** with integrity verification
- **CD-Text and ISRC extraction**
- **CUE sheet generation**
- **Disc fingerprinting** — CDDB, MusicBrainz, and AccurateRip disc IDs

### Disc Diagnostics
- **C2 error scan** — quick pass/fail quality check
- **BLER scan** — detailed per-second error rate with Red Book compliance check
- **Disc rot detection** — two-phase spatial degradation pattern analysis
- **Comprehensive scan** — all tests combined into a single scored report (A–F)
- **Surface map** — per-sector C2 error CSV for external visualization
- **Speed comparison test** — reads sectors at two speeds to detect surface instability
- **Multi-pass verification** — reads sectors N times to detect read inconsistency
- **Lead-in/lead-out area check** — scans the first/last 150 sectors for edge damage
- **Audio content analysis** — detects silent, clipped, low-level, and DC-offset sectors
- **Seek time analysis** — measures seek latency to detect mechanical issues
- **C2 validation test** — verifies that the drive's C2 error reporting is reliable

---

## Quality Scan Modes

AudioCopy offers three primary quality scans. Each answers a different question about a disc.

### C2 Error Scan (Quick)

**Question answered:** *"Does this disc have uncorrectable errors?"*

A CD drive performs two internal error-correction stages. **C1** corrects minor errors transparently. **C2** is the second and final stage — a C2 error means the drive's hardware could not fully correct the data and the returned audio samples may be wrong.

This scan reads every audio sector once and asks the drive to report C2 error pointers. It supports four sensitivity modes:

| Mode | Behavior |
|---|---|
| **Standard** | Single pass, byte-level C2 counting |
| **PlexTools-style** | Single pass with cache defeat, then conditional re-read of error sectors |
| **Multi-pass** | Two-pass scan without cache defeat (faster) |
| **Paranoid** | Cache defeat + conditional three-pass re-read of error sectors |

Optionally performs **dual-speed validation** — re-reads error sectors at a different speed to distinguish real media errors from speed-dependent read artifacts.

**Output:** Total C2 count, per-sector error counts, error LBA list, and a summary report.

**When to use:** Quick check before ripping to decide whether standard or secure mode is needed.

---

### BLER Scan (Detailed)

**Question answered:** *"What is the error rate over time, and does it meet Red Book standards?"*

**BLER** (Block Error Rate) is an IEC 60908 (Red Book) concept. The standard defines maximum acceptable error rates measured per second of audio playback. AudioCopy reads every audio sector, counts C2 errors, and aggregates them into one-second time buckets (75 sectors = 1 second at 1× CD speed). The result is a complete time-series error profile of the disc.

| Metric | Description |
|---|---|
| **Avg C2/sec** | Mean C2 errors per second across the entire disc |
| **Max C2/sec** | Peak one-second error count (with timestamp) |
| **Red Book threshold** | Avg C2/sec < 220 = PASS (IEC 60908 compliance) |
| **Quality threshold** | Avg C2/sec < 1.0 = GOOD for archival ripping |
| **Per-track breakdown** | Error count, affected sectors, avg/sec, and status per track |
| **ASCII error graph** | Visual distribution of errors across the disc timeline |

**Rating scale:**

| Rating | Criteria |
|---|---|
| **EXCELLENT** | Zero C2 errors |
| **GOOD** | Avg < 1.0/sec, longest error run < 3 sectors |
| **ACCEPTABLE** | Avg < 10.0/sec, longest error run < 10 sectors |
| **POOR** | Above acceptable thresholds |
| **BAD** | Read failures occurred (sectors could not be read at all) |

**Output:** Full report printed to console, plus a CSV log (`bler_scan.csv`) with per-second LBA and error count for graphing in external tools.

**When to use:** Detailed quality assessment — see exactly where errors are, whether the disc meets Red Book limits, and whether it is safe to archive.

---

### Disc Rot Detection

**Question answered:** *"Is this disc physically degrading, and how urgently should I back it up?"*

Disc rot is the chemical or physical deterioration of a CD's reflective aluminum layer. It produces **characteristic spatial error patterns** that are distinct from scratches, fingerprints, or manufacturing defects. A disc can have zero C2 errors on a single read pass and still be in early-stage rot, detectable only through read instability across multiple passes.

AudioCopy performs a two-phase scan:

**Phase 1 — C2 error distribution**
Reads the entire disc and classifies every sector into three radial zones:

| Zone | Disc position |
|---|---|
| **Inner** | 0–33% (near the center hub) |
| **Middle** | 33–66% |
| **Outer** | 66–100% (near the outer edge) |

Error rates are computed per zone to reveal spatial concentration patterns.

**Phase 2 — Adaptive read consistency**
Re-reads sampled sectors multiple times (3 passes per sample) to detect **read instability** — the same sector returning different audio data on different reads. The sampling density adapts per zone: zones with higher error rates from Phase 1 receive denser sampling (down to every 20th sector) while clean zones are sampled sparsely (every 200th sector).

The scan then evaluates four degradation indicators:

| Indicator | Detection rule | What it means |
|---|---|---|
| **Edge concentration** | Outer error rate > 2× inner rate and > 1% | Rot typically starts at the disc edge where the protective lacquer is thinnest |
| **Progressive pattern** | Error rate increases monotonically inner → middle → outer, outer > 0.5% | Classic inward-spreading rot progression |
| **Pinhole pattern** | > 10 small clusters (≤ 3 sectors) comprising > 50% of all clusters | Microscopic holes in the reflective layer caused by oxidation |
| **Read instability** | > 5% of re-read samples return different data | The reflective layer is intermittently unreadable — data is being lost |

A weighted scoring system produces the final risk level:

| Indicator | Weight |
|---|---|
| Edge concentration | +25 |
| Progressive pattern | +25 |
| Read instability | +20 |
| Pinhole pattern | +15 |
| Inconsistency rate > 10% | +15 |

| Score | Risk level |
|---|---|
| 0–9 | **NONE** — Disc appears healthy |
| 10–29 | **LOW** — Minor issues, consider backing up soon |
| 30–49 | **MODERATE** — Early degradation, back up immediately |
| 50–74 | **HIGH** — Significant degradation, back up NOW |
| 75–100 | **CRITICAL** — Severe damage, extract whatever data is possible |

**Output:** Zone error rates, cluster analysis, indicator flags, risk assessment, and a recommendation. Saved as a text report (`discrot_report.txt`).

**When to use:** When you suspect physical deterioration (visible bronzing, edge discoloration, age > 15 years) and need to know whether data loss is imminent.

---

## C2/BLER vs. Disc Rot — Summary of Differences

| | C2 Scan | BLER Scan | Disc Rot Detection |
|---|---|---|---|
| **Question** | Are there uncorrectable errors? | What is the error rate over time? | Is the disc physically degrading? |
| **Read passes** | 1–3 (configurable) | 1 | Full disc + adaptive multi-pass sampling |
| **Spatial analysis** | No | No (time-series only) | Yes — three-zone radial classification |
| **Read consistency** | Not tested | Not tested | Multi-pass re-read detects instability |
| **Pattern analysis** | None | Per-second bucketing, per-track totals | Edge concentration, progressive gradient, pinhole clusters |
| **Typical cause detected** | Scratches, fingerprints, poor burns | Same as C2 but with temporal context | Chemical oxidation, delamination, bronzing |
| **Output format** | Console report | Console report + CSV + ASCII graph | Console report + text log |
| **Actionable result** | "Use secure rip" or "clean the disc" | "Meets/fails Red Book" or "use Paranoid mode" | "Back up NOW — data loss imminent" |
| **Speed** | Fast (minutes) | Moderate (full disc read) | Slow (full disc read + re-read sampling) |

**Key insight:** A disc can pass a C2 scan with zero errors yet still be in early-stage rot — Phase 2's read consistency check catches degradation that a single read pass cannot. Conversely, a disc with high BLER from a surface scratch will show **no rot indicators** because the damage is mechanical, not chemical.

---

## Comprehensive Scan

For a single combined assessment, the **Comprehensive Scan** (menu option in-app) runs all five tests sequentially — BLER, disc rot, speed comparison, multi-pass verification, and audio content analysis — and produces a weighted **0–100 score** with a letter grade:

| Grade | Score |
|---|---|
| **A** | 90–100 |
| **B** | 80–89 |
| **C** | 70–79 |
| **D** | 60–69 |
| **F** | 0–59 |

Estimated runtime: 15–30 minutes depending on disc condition and drive speed.

---

## Building

Requires **Visual Studio** with the **C++ Desktop Development** workload. Open `AudioCopy.vcxproj` and build. No external dependencies beyond the Windows SDK (`winhttp.lib` is used for AccurateRip lookups and is included in the SDK).

---

## Usage

Insert an audio CD. AudioCopy auto-detects drives, reads the TOC, queries AccurateRip, and presents an interactive menu:

Press **ESC** or **Ctrl+C** at any time to cancel a running operation.

---

## Acknowledgments

- **[AccurateRip](http://www.accuraterip.com/)** — AudioCopy calculates AccurateRip V1 checksums and queries the AccurateRip online database to verify rip accuracy against submissions from other users worldwide. The AccurateRip database and protocol were created by Illustrate, the developers of dBpoweramp.

- **[Exact Audio Copy (EAC)](https://www.exactaudiocopy.de/)** by André Wiethoff — the pioneering CD ripper that defined secure ripping methodology. EAC established the practices of multi-pass sector reading, C2 error pointer detection, drive cache defeat, read offset correction, overreading into lead-in/lead-out, and paranoid-mode extraction with bit-level verification. AudioCopy's secure rip implementation, error handling strategy, and overall approach to verifiable extraction are directly informed by the standards EAC set.

- **[dBpoweramp](https://www.dbpoweramp.com/)** by Illustrate — created the AccurateRip system and popularized the concept of verifying CD rips against a shared online database of checksums. dBpoweramp's approach to automatic drive offset detection, C2 error reporting, and its emphasis on making verified ripping accessible to a broad audience established industry-wide expectations for audio CD extraction software.

EAC and dBpoweramp together defined the modern standard for verifiable, bit-perfect audio CD extraction. AudioCopy builds on the methodology and concepts they established.

---

## License

This project is provided as-is for personal and educational use.
