# AudioCopy
"You take the blue pill—the story ends, you wake up in your bed and believe whatever you want to believe.
You take the red pill—you stay in Wonderland, and I show you how deep the codebase goes."

A Windows command-line tool for high-quality audio CD ripping, writing, and advanced disc diagnostics, written in C++.

AudioCopy reads and writes audio CDs at the raw sector level using SCSI/MMC commands and provides multiple quality scanning modes to assess disc health before, during, or after extraction.

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

### Disc Writing
- **Write audio CDs** from `.bin` / `.cue` / `.sub` file sets
- **Automatic write mode negotiation** — tries Raw DAO with subchannel, falls back to SAO
- **Subchannel writing** — writes packed or raw P-W subchannel data from `.sub` files when the drive supports it
- **CD-Text writing** — builds and sends CD-Text packs (Title, Performer) from CUE sheet metadata
- **CD-RW detection and blanking** — detects rewritable media, supports quick and full blank with progress tracking
- **Optical Power Calibration (OPC)** — optional laser power calibration before writing
- **Write verification** — cache flush, session close, lead-out finalization, and post-write sector readback
- **Configurable write speed** with drive speed selection

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
- **Copy-protection detection** — heuristic scan for common audio CD protection schemes

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

#### C1 Block Error Reporting

A CD drive performs two internal error-correction stages. **C1** corrects minor errors transparently — every disc has some C1 activity, even in perfect condition. **C2** is the second and final stage — a C2 error means the drive's hardware could not fully correct the data.

Some drives expose per-sector C1 block error counts in bytes 294–295 of the C2 error pointer response. AudioCopy auto-detects this at startup by probing sample sectors across the disc. When available, the BLER report includes full C1 statistics alongside C2, giving a much more complete picture of disc health — a disc can have zero C2 errors but elevated C1 rates indicating early wear.

| Drive type | C1 reporting | Detection method |
|---|---|---|
| **Plextor (D8-capable)** | Always available | Vendor command `0xD8` returns C1/C2 block error stats |
| **LiteOn, ASUS, Pioneer** | Often available | Standard MMC ErrorPointers mode, bytes 294–295 probed |
| **Other MMC drives** | Auto-detected | Probes 150 sectors across 3 zones; reports C1 if any byte 294 is non-zero |
| **ErrorBlock-only drives** | Not available | Different data layout; C1 bytes are not present |

#### Metrics

| Metric | Description |
|---|---|
| **Avg C1/sec** | Mean C1 corrections per second (when available) |
| **Max C1/sec** | Peak one-second C1 count |
| **Avg C2/sec** | Mean C2 errors per second across the entire disc |
| **Max C2/sec** | Peak one-second error count (with timestamp) |
| **Red Book threshold** | Avg BLER < 220/sec = PASS (IEC 60908 compliance) |
| **Quality threshold** | Avg C2/sec < 1.0 = GOOD for archival ripping |
| **Per-track breakdown** | C1 count, C2 count, affected sectors, avg/sec, and status per track |
| **ASCII error graph** | Visual distribution of C2 errors across the disc timeline |

#### C1 Assessment Scale

| C1 Avg/sec | Assessment |
|---|---|
| < 5 | **EXCELLENT** — minimal correction needed |
| 5–50 | **GOOD** — normal wear |
| 50–220 | **FAIR** — elevated but within Red Book limits |
| > 220 | **POOR** — exceeds Red Book BLER limit |

#### Overall Quality Rating

| Rating | Criteria |
|---|---|
| **EXCELLENT** | Zero C2 errors |
| **GOOD** | Avg < 1.0/sec, longest error run < 3 sectors |
| **ACCEPTABLE** | Avg < 10.0/sec, longest error run < 10 sectors |
| **POOR** | Above acceptable thresholds |
| **BAD** | Read failures occurred (sectors could not be read at all) |

**Output:** Full report printed to console, plus a CSV log (`bler_scan.csv`) with per-second LBA and error count for graphing in external tools.

**When to use:** Detailed quality assessment — see exactly where errors are, whether the disc meets Red Book limits, and whether it is safe to archive. The C1 data (when available) reveals early degradation that C2-only scans miss entirely.

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
| **C1 reporting** | No | Yes (auto-detected per drive) | No |
| **Read passes** | 1–3 (configurable) | 1 | Full disc + adaptive multi-pass sampling |
| **Spatial analysis** | No | Zone distribution (inner/middle/outer) | Yes — three-zone radial classification |
| **Read consistency** | Not tested | Not tested | Multi-pass re-read detects instability |
| **Pattern analysis** | None | Per-second bucketing, per-track totals, error clustering | Edge concentration, progressive gradient, pinhole clusters |
| **Typical cause detected** | Scratches, fingerprints, poor burns | Same as C2 but with temporal context + early wear via C1 | Chemical oxidation, delamination, bronzing |
| **Output format** | Console report | Console report + CSV + ASCII graph | Console report + text log |
| **Actionable result** | "Use secure rip" or "clean the disc" | "Meets/fails Red Book" or "use Paranoid mode" | "Back up NOW — data loss imminent" |
| **Speed** | Fast (minutes) | Moderate (full disc read) | Slow (full disc read + re-read sampling) |

**Key insight:** A disc can pass a C2 scan with zero errors yet still be in early-stage rot — Phase 2's read consistency check catches degradation that a single read pass cannot. The BLER scan's C1 data (when available) fills the gap between "zero C2" and "actual disc health" — elevated C1 rates reveal wear that hasn't yet progressed to uncorrectable errors. Conversely, a disc with high BLER from a surface scratch will show **no rot indicators** because the damage is mechanical, not chemical.

---


## Subchannel Data Extraction

### What is subchannel data?

Every CD sector contains two separate data streams read simultaneously by the laser:

| Channel | Size per sector | Content | Error correction |
|---|---|---|---|
| **Main channel** | 2,352 bytes | Audio samples (or data) | CIRC — two layers of Reed-Solomon (C1 + C2) with interleaving |
| **Subchannel** | 96 bytes | Navigational metadata split across 8 sub-channels (P, Q, R, S, T, U, V, W) | None — only a 16-bit CRC on the Q channel |

The **P channel** carries a simple pause/play flag. The **Q channel** carries track number, index, and MSF timestamps — the data a CD player uses for its display. The **R–W channels** are optional and carry CD-G graphics (karaoke), CD-TEXT, or are simply empty.

### When to enable subchannel extraction

When ripping, AudioCopy asks whether to include subchannel data. Enabling it creates an additional `.sub` file alongside the `.bin` image. Use the following guidelines:

| Scenario | Include subchannel? | Reason |
|---|---|---|
| **Archival / preservation rip** | **Yes** | Preserves the complete disc image including all metadata channels. A `.bin`+`.sub`+`.cue` set is a bit-perfect representation of the original disc. |
| **CD-G / karaoke disc** | **Yes** | The R–W channels contain the graphics data. Without subchannel extraction, the karaoke visuals are lost. |
| **CD-TEXT disc** | **Yes** | Artist and title metadata encoded in the R–W subchannel is preserved. |
| **Disc with hidden tracks or non-standard indexing** | **Yes** | Raw Q-channel data captures index points and timing that may not be fully represented in the TOC. |
| **Pressed / factory disc (standard audio)** | **Optional** | Pressed CDs always have valid P+Q subchannel data, but the R–W channels are usually empty on standard audio CDs. Extraction preserves timing metadata but adds no audio content. |
| **Burned CD-R (standard audio)** | **No** | Most CD-R burns do not write meaningful R–W subchannel data. The P+Q timing is generated automatically by the drive and is typically less reliable than the TOC. |
| **Quick rip for personal listening** | **No** | Subchannel data is not needed for playback. Skipping it produces a smaller output and slightly faster rips. |

### How to decide

If you are unsure whether a disc has subchannel content worth preserving, run **option 11 — Verify Subchannel Burn Status** before ripping. This samples sectors across the disc and reports:

- Whether Q-channel CRC data is valid (indicating reliable subchannel data was mastered/burned)
- Whether R–W channels contain CD-G graphics or CD-TEXT metadata
- A clear recommendation on whether to enable subchannel extraction

**Rule of thumb:** If you are archiving and storage is not a concern, always include subchannel data — it costs ~4% extra file size and ensures nothing is lost. If you are ripping for playback only, skip it.

# Subchannel Data on Burned Audio CDs: Why R–W Is Often Empty and P+Q Gets Auto-Generated

This README explains why **R–W subchannel data is usually missing** on burned audio CDs, why **P+Q timing is commonly generated automatically**, and why the **TOC is typically more reliable** for navigation.

---

## 1) R–W Subchannel Data (Meaningful Content)

### What it is
The **R–W channels** (subchannels **3 through 8**) *can* carry meaningful data such as:

- **CD+G graphics** (commonly used for karaoke)
- **CD-Text** (in some implementations)

### Why it's often missing
Most consumer CD burning software defaults to **not writing** R–W subchannel content unless explicitly configured. Examples include:

- Nero
- ImgBurn
- iTunes
- Finder / Windows Explorer

These tools generally prioritize writing the **audio** and the **core control/timing subchannels** (especially P+Q). As a result, they often write **generic, blank, or repeating** patterns in R–W instead of real CD+G or mastered subchannel data.

### Outcome
On typical burned audio CDs:

- Players rarely use R–W during playback.
- Players rely on the **Table of Contents (TOC)** for track start/end times.
- For an audio CD that has CD-TEXT, the CD-TEXT data is commonly stored in the lead-in area (before track 1). So the location is often lead-in, but the mechanism is still subchannel R-W.

---

## 2) P+Q Timing Generation (Automatic)

### What it is
The **Q channel** contains critical low-level timing and identification info, such as:

- track numbers
- timestamps (relative/absolute time)
- track/index boundaries

### The issue
During common burn modes like:

- **TAO (Track-At-Once)**
- quick **DAO (Disc-At-Once)**

…the drive often **generates Q-subchannel codes on the fly** based on the incoming stream, rather than using a pre-authored mastering file containing precise subchannel timing.

### Result
Because timing is created in real time:

- it can be subject to small inconsistencies ("jitter")
- it's usually fine for audio playback
- but it can sometimes trip up:
  - very picky older CD players
  - certain data recovery or verification workflows that expect highly consistent subchannel timing

---

## 3) TOC vs. Q-Subchannel Reliability

### TOC (Table of Contents)
- Stored in the **Lead-In** at the beginning of the disc
- Written at the **end of the burn process**
- Fixed and generally read very reliably by drives/players
- Used to determine where tracks start and end

### P+Q timing
- Updated continuously across the burn
- A brief write/reading issue can create a **Q-channel glitch**
- This can happen even when the audio data is still recoverable/corrected via ECC

### Conclusion
- **TOC** is generally the most reliable for **disc navigation**
- **Q-subchannel** is heavily used for **playback tracking** and timing, but can be more vulnerable to momentary glitches

---

## Summary

For standard audio CD burns, these behaviors explain why:

- some players show a "CD+G" label or track name while others don't
- burned discs may take longer to load than pressed discs
- special features like CD+G graphics often don't work unless explicitly authored and written

If you need maximum compatibility for special features (especially **CD+G**), use software/hardware that supports:

- **RAW DAO (Disc-At-Once)**, and
- explicit inclusion of **R–W subchannel data**


### Output files

| Subchannel setting | Files created |
|---|---|
| **Include** | `.bin` (audio) + `.sub` (96 bytes/sector raw subchannel) + `.cue` (sheet) |
| **Audio only** | `.bin` (audio) + `.cue` (sheet) |

---

### Subchannel Integrity Verification

**Question answered:** *"Is the Q subchannel data on this disc readable, and could it affect track boundary detection?"*

The Q subchannel is a narrow metadata channel embedded alongside the audio data on every CD sector. It carries the current track number, index point, and timestamps — the information a CD player uses for its real-time display ("Track 3, 2:47"). AudioCopy reads and validates this data for every audio sector on the disc.

#### Why subchannel errors are expected

Unlike audio data, subchannel data has **no error correction**:

| Property | Audio Data | Q Subchannel |
|---|---|---|
| **Error correction** | CIRC — two layers of Reed-Solomon (C1 + C2) with interleaving | None — only a 16-bit CRC for detection |
| **Interleaving** | Yes — data is spread across ~100 frames to survive scratches | No — each 96-bit frame stands alone |
| **Redundancy** | ~25% of raw channel data is parity bytes | Zero — 10 bytes payload + 2 bytes CRC |
| **On read failure** | Hardware reconstructs the original samples perfectly | Data is simply lost — no recovery possible |

A single bit flip anywhere in a 96-bit subchannel frame causes the CRC to fail. The same bit flip in the audio channel would be silently corrected by the C1/C2 error correction hardware before the data ever reaches software.

**A subchannel error rate of 1–3% is normal and expected on most CDs, even brand-new pressed discs.** This is not a defect — it reflects the physical limitations of a channel that was designed as best-effort navigational metadata, not as a reliable data transport.

#### What the scan checks

For every audio sector, AudioCopy:

1. Reads the raw 96-byte interleaved subchannel data via `READ CD` (subchannel mode 01h)
2. De-interleaves the Q channel bits and validates the CRC-16-CCITT checksum
3. If the raw CRC fails, retries once (transient errors are common)
4. If raw reading fails twice, falls back to the drive's formatted Q subchannel (mode 02h)
5. If a valid Q frame is recovered, validates that the reported track number matches the expected track from the TOC

Errors are classified into three categories:

| Error type | Meaning |
|---|---|
| **CRC/Read errors** | The Q subchannel CRC failed on both raw attempts and the formatted fallback also failed — the data for this sector is unrecoverable |
| **Track mismatches** | The Q data was read and CRC-verified successfully, but the reported track number does not match the expected track from the TOC. Common at track boundaries and pregaps |
| **Index errors** | The decoded index value is outside the valid BCD range (0–99) |

The scan also tracks **burst errors** — consecutive sectors with failures — to identify localized damage versus uniformly distributed noise.

#### Interpreting results

| Error rate | Assessment |
|---|---|
| **0%** | Exceptionally clean — uncommon even on new discs |
| **< 1%** | Excellent — no impact on ripping |
| **1–3%** | Normal — typical baseline for most CDs and drives |
| **3–5%** | Elevated — may indicate disc wear, but audio extraction is unaffected |
| **5–10%** | High — disc surface may be degraded; cross-reference with C2 scan |
| **> 10%** | Severe — likely physical damage; prioritize backup with secure rip mode |

#### Why this does not affect audio quality

The subchannel and the audio data are physically separate channels on the disc. A subchannel CRC failure means the 96-bit navigational frame for that sector was unreadable — it says nothing about the 2,352-byte audio payload, which has its own independent and far more robust error correction.

AudioCopy (and all modern rippers) determines track boundaries from the **Table of Contents** in the disc lead-in, not from per-sector subchannel data. The subchannel is useful for:

- Detecting index points within tracks (e.g., hidden tracks in pregaps)
- Extracting ISRC codes and MCN (Media Catalog Number)
- Verifying that the TOC and the on-disc metadata are consistent

A high subchannel error rate is a signal to inspect the disc further (run a C2 or disc rot scan), but it does not indicate that the extracted audio will contain errors.

**Output:** Sector count, per-category error totals, burst analysis, and measured read speed.

**When to use:** Before ripping discs where index point accuracy matters (live albums, gapless recordings), or as a general disc health indicator to decide whether further diagnostics are warranted.

---

## Disc Writing

AudioCopy can write audio CDs from `.bin` / `.cue` / `.sub` file sets produced by a previous rip. The write workflow handles media detection, mode negotiation, CD-Text embedding, and session finalization automatically.

### Write Mode Negotiation

Not all drives support the same write modes. AudioCopy probes the drive by testing both MODE SELECT and SEND CUE SHEET together, since some drives accept the mode page but reject the CUE sheet. The negotiation tries modes in priority order:

| Priority | Mode | Block size | Description |
|---|---|---|---|
| 1 | Raw DAO + packed P-W | 2448 bytes | Exact 1:1 copy including subchannel data (best fidelity) |
| 2 | Raw DAO + raw P-W | 2448 bytes | Raw subchannel format (deinterleaving handled by host) |
| 3 | SAO + packed P-W | 2448 bytes | Session-At-Once with packed subchannel |
| 4 | SAO + raw P-W | 2448 bytes | Session-At-Once with raw subchannel |
| 5 | Plain SAO | 2352 bytes | Audio only — drive generates subchannel automatically (last resort) |

If the drive silently downgrades the requested write parameters (accepts MODE SELECT but stores different values), AudioCopy detects this via readback verification and rejects the mode.

### Subchannel Writing

When a `.sub` file is provided alongside the `.bin` and `.cue`, AudioCopy validates it against the expected sector count and writes subchannel data interleaved with the audio. Raw P-W subchannel data is automatically deinterleaved to packed format when required by the negotiated write mode.

If the drive does not support any subchannel write mode, AudioCopy falls back to plain SAO and writes audio only, with a warning that subchannel data will be omitted.

### CD-Text Embedding

If the CUE file contains `TITLE` and/or `PERFORMER` commands (at disc and/or track level), AudioCopy automatically:

1. Builds CD-Text packs (pack types 0x80 Title, 0x81 Performer, 0x8F Size Information)
2. Computes CRC-16 (CRC-CCITT, inverted per Red Book) for each 18-byte pack
3. Sends the packs to the drive via WRITE BUFFER (buffer ID 0x08)

The drive embeds the CD-Text in the lead-in R-W subchannel during SAO/DAO writing. If the drive rejects the CD-Text data, the audio is still written normally.

### CD-RW Detection and Blanking

Before writing, AudioCopy queries the disc via READ DISC INFORMATION (0x51) to detect:

- **Media type** — CD-R (write-once) or CD-RW (rewritable)
- **Disc status** — empty, appendable, or complete (full)

If the primary command fails (e.g., corrupted TOC), a fallback via GET CONFIGURATION (0x46) determines the media profile.

For CD-RW media that needs erasing, two blanking modes are available:

| Mode | SCSI blank type | Behavior |
|---|---|---|
| **Quick blank** | 0x01 (minimal) | Erases PMA, lead-in, and pregap only (~1–2 minutes) |
| **Full blank** | 0x00 (entire disc) | Erases the entire disc surface (~10+ minutes) |

If the standard blank fails (e.g., corrupted TOC prevents the drive from processing a normal blank), AudioCopy automatically attempts a recovery blank via erase session (type 0x06), then retries the original blank. Progress is tracked via REQUEST SENSE polling with a real-time progress bar.

### Write Workflow

The full write sequence is:

1. **File validation** — verify `.bin`, `.cue`, and optional `.sub` files exist and are consistent
2. **CUE sheet parsing** — extract track layout, pregap data, ISRC codes, and CD-Text metadata
3. **Power calibration** — optional OPC (SEND OPC INFORMATION, 0x54)
4. **Write mode negotiation** — probe drive capabilities and select best supported mode
5. **SCSI CUE sheet** — build and send the disc layout (SEND CUE SHEET, 0x5D) with Track 1 pregap at MSF 00:00:00
6. **CD-Text** — build and send packs if CUE metadata is present
7. **Audio sector writing** — write 150 sectors of pregap silence followed by BIN file data via WRITE(10), with subchannel data appended when available
8. **Finalization** — SYNCHRONIZE CACHE (0x35), CLOSE SESSION (0x5B), and lead-out polling until the drive is ready

### Post-Write Verification

After finalization, AudioCopy can verify the written disc by reading back the first and last sector of each track to confirm readability.

---

## Copy-Protection Detection

**Question answered:** *"Is this disc copy-protected, and what scheme is being used?"*

AudioCopy performs an 8-step heuristic scan that combines structural analysis (no disc I/O) with targeted reads to detect common audio CD copy-protection mechanisms.

### Checks Performed

| Step | Check | I/O required | Severity | What it detects |
|---|---|---|---|---|
| 1 | **Illegal TOC** | No | Strong | Track numbers outside 1–99, impossibly high start LBAs |
| 2 | **Multi-session abuse** | No | Strong | Session count > 2 (used by MediaMax/XCP to confuse rippers) |
| 3 | **Data track presence** | No | Strong | Non-audio track in last session (rootkit installer, autorun) |
| 4 | **Pre-emphasis anomaly** | No | Weak | Pre-emphasis flag set inconsistently across tracks |
| 5 | **Track gap anomalies** | No | Weak | Non-standard gap sizes between tracks |
| 6 | **Intentional errors** | Yes | Strong | Clusters of C2 / read errors deliberately mastered onto the disc (CDS, MediaClyS) |
| 7 | **Subchannel manipulation** | Yes | Strong | Corrupted or manipulated subchannel data patterns |
| 8 | **Lead-in overread block** | Yes | Strong | Drive refuses to read lead-in area (some protections block this) |

### Verdict Logic

The scan classifies each indicator as **strong** (severity ≥ 2) or **weak** (severity < 2) and applies the following rules:

| Condition | Verdict |
|---|---|
| No indicators detected | **No protection** |
| Only weak indicators | **Unlikely** — minor anomalies, not protection |
| 1 strong, no weak | **Inconclusive** — possible but not confirmed |
| 1 strong + 1 weak | **Inconclusive** — insufficient corroborating evidence |
| ≥ 2 strong, or ≥ 1 strong + ≥ 2 weak | **Protection likely** — specific scheme identified if possible |

### Identified Schemes

When sufficient indicators are present, AudioCopy attempts to identify the specific protection scheme:

| Indicator combination | Identified scheme |
|---|---|
| Data track + multi-session | MediaMax / XCP-style |
| Intentional errors + illegal TOC | Cactus Data Shield / Key2Audio-style |
| Intentional errors alone | CDS / MediaClyS-style |
| Subchannel manipulation | Subchannel-based protection |

**Output:** Per-indicator results, aggregate verdict, identified scheme (if any), and a text report saved to `protection_check.txt`.

**When to use:** Before ripping unfamiliar discs — particularly commercial releases from 2001–2007 when audio CD copy-protection was widespread. The scan helps decide whether to use secure rip mode and warns about potential extraction issues.

---

## Pre-gap Scanning

AudioCopy detects pre-gaps (INDEX 00 regions) on audio CDs using a two-phase algorithm with backward refinement. Pre-gaps are the audio segments (often silent) that exist between INDEX 00 and INDEX 01 of each track.

### Why Pre-gap Detection Matters

CD subchannel data can be unreliable, with drives frequently reporting stale or incorrect index values near track boundaries. A naive sector-by-sector scan would be both slow and prone to false positives. AudioCopy's approach prioritizes accuracy over speed by scanning every sector with majority-voted subchannel reads, then refining the result backward to compensate for drive latency.

### The Algorithm

#### Phase 1: Fine Scan (Precise Boundary Detection)
- **Step Size**: 1 sector (sector-by-sector)
- **Range**: Up to 450 sectors before the track's INDEX 01 boundary (Track 1 scans from LBA 0)
- **Reliability**: Uses `ReadSectorQ()` with 3-round majority voting to filter out spurious subchannel reads
- **Validation**: Requires **≥3 consecutive INDEX 0 hits** to accept a boundary
  - Isolated spurious subchannel values are rejected
  - Prevents false positives from stale Q subchannel data
- **Early Exit**: Stops scanning once INDEX 1 is detected after a confirmed boundary

#### Phase 2: Backward Refinement (Compensating for Read Displacement)
- **Range**: Checks up to **8 sectors backward** from the detected boundary
- **Purpose**: Compensate for subchannel read displacement (drives often report index changes several sectors late)
- **Method**: Probes backward sector-by-sector using majority-voted reads until a non-INDEX 0 sector is found
- **Result**: Captures the true start of the pre-gap region

### Scan Parameters

- **Drive Speed**: Reduced to 4× during pre-gap scanning for improved subchannel reliability
- **Track 1 Special Case**: Scans from LBA 0 (disc start) since Track 1 pre-gaps may begin at the very start of the disc
- **Other Tracks**: Scans up to **450 sectors backward** (~6 seconds) to accommodate:
  - Standard pre-gaps (typically 150 frames / 2 seconds)
  - Non-standard pre-gaps on live albums or gapless discs

This algorithm ensures accurate pre-gap detection across a wide variety of CD pressings and drive models, even when subchannel data quality is poor.

### Output

The detected pre-gap boundary is stored in `disc.tracks[i].pregapLBA` and reported to the user:
---

## Building

Requires **Visual Studio** with the **C++ Desktop Development** workload. Open `AudioCopy.vcxproj` and build. No external dependencies beyond the Windows SDK (`winhttp.lib` is used for AccurateRip lookups and is included in the SDK).

---

## Usage

Insert an audio CD. AudioCopy auto-detects drives, reads the TOC, queries AccurateRip, and presents an interactive menu:

Press **ESC** or **Ctrl+C** at any time to cancel a running operation.

---

## License

This project is provided as-is for personal and educational use.

## Acknowledgments

- **cdda2wav (cdrtools)** — a long-standing reference implementation for secure CD audio extraction. Its work on drive handling, offset behavior, and reliable digital audio extraction helped shape the field AudioCopy builds upon.

- **cdparanoia / paranoia libraries** — influential for robust read-verification strategies and jitter correction. AudioCopy's paranoid and multi-pass verification concepts draw from the reliability goals established by the paranoia toolset.

- **[AccurateRip](http://www.accuraterip.com/)** — AudioCopy calculates AccurateRip V1 checksums and queries the AccurateRip online database to verify rip accuracy against submissions from other users worldwide. The AccurateRip database and protocol were created by Illustrate, the developers of dBpoweramp.

- **[Exact Audio Copy (EAC)](https://www.exactaudiocopy.de/)** by André Wiethoff — the pioneering CD ripper that defined secure ripping methodology. EAC established the practices of multi-pass sector reading, C2 error pointer detection, drive cache defeat, read offset correction, overreading into lead-in/lead-out, and paranoid-mode extraction with bit-level verification. AudioCopy's secure rip implementation, error handling strategy, and overall approach to verifiable extraction are directly informed by the standards EAC set.

- **[dBpoweramp](https://www.dbpoweramp.com/)** by Illustrate — created the AccurateRip system and popularized the concept of verifying CD rips against a shared online database of checksums. dBpoweramp's approach to automatic drive offset detection, C2 error reporting, and its emphasis on making verified ripping accessible to a broad audience established industry-wide expectations for audio CD extraction software.

EAC and dBpoweramp together defined the modern standard for verifiable, bit-perfect audio CD extraction. AudioCopy builds on the methodology and concepts they established.