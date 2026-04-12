// ============================================================================
// TrackRipWorkflow.cpp - Rip individual tracks to WAV or FLAC
//
// Workflow: track selection → format (WAV/FLAC) → speed → burst/safe mode →
// verification prompt → read disc → save files → optional physical compare.
//
// FLAC output requires flac.exe on the system PATH.  If not found the track
// is saved as WAV and the user is notified.
// ============================================================================
#define NOMINMAX
#include "TrackRipWorkflow.h"
#include "ConsoleColors.h"
#include "FileUtils.h"
#include "InterruptHandler.h"
#include "MenuHelpers.h"
#include "Progress.h"
#include <algorithm>
#include <conio.h>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>
#include <windows.h>

// ═══════════════════════════════════════════════════════════════════════════
//  WAV / FLAC file writers
// ═══════════════════════════════════════════════════════════════════════════

static bool Utf8ToWide(const std::string& input, std::wstring& output)
{
	int wlen = MultiByteToWideChar(CP_UTF8, 0, input.c_str(), -1, nullptr, 0);
	if (wlen <= 0) {
		output.clear();
		return false;
	}

	std::wstring wide(static_cast<size_t>(wlen), L'\0');
	int converted = MultiByteToWideChar(CP_UTF8, 0, input.c_str(), -1, wide.data(), wlen);
	if (converted <= 0) {
		output.clear();
		return false;
	}

	if (!wide.empty() && wide.back() == L'\0')
		wide.pop_back();

	output = std::move(wide);
	return true;
}

static bool TrackHadReadErrors(const DiscInfo& disc, const TrackInfo& track)
{
	DWORD readStart = (disc.pregapMode == PregapMode::Skip) ? track.startLBA : track.pregapLBA;
	for (DWORD lba : disc.badSectors) {
		if (lba >= readStart && lba <= track.endLBA)
			return true;
	}
	return false;
}

// Writes a standard 44-byte RIFF/WAVE file (16-bit stereo 44100 Hz PCM).
 // Sectors are batched into a write buffer to reduce I/O call overhead.
static bool WriteWavFile(const std::wstring& path,
	const std::vector<std::vector<BYTE>>& sectors,
	size_t startSector, size_t sectorCount)
{
	if (sectorCount == 0) return false;
	if (startSector >= sectors.size()) return false;
	if (sectorCount > (sectors.size() - startSector)) return false;

	for (size_t i = 0; i < sectorCount; i++) {
		if (sectors[startSector + i].size() < AUDIO_SECTOR_SIZE)
			return false;
	}

	unsigned long long dataSize64 =
		static_cast<unsigned long long>(sectorCount) * AUDIO_SECTOR_SIZE;
	if (dataSize64 > 0xFFFFFFFFull - 36ull)
		return false;

	std::ofstream out(path, std::ios::binary);
	if (!out) return false;

	uint32_t dataSize = static_cast<uint32_t>(dataSize64);
	uint32_t fileSize = dataSize + 36;
	uint16_t audioFmt = 1, ch = 2, bps = 16;
	uint32_t rate = 44100;
	uint16_t blockAlign = ch * (bps / 8);
	uint32_t byteRate = rate * blockAlign;
	uint32_t fmtSize = 16;

	out.write("RIFF", 4);
	out.write(reinterpret_cast<const char*>(&fileSize), 4);
	out.write("WAVE", 4);
	out.write("fmt ", 4);
	out.write(reinterpret_cast<const char*>(&fmtSize), 4);
	out.write(reinterpret_cast<const char*>(&audioFmt), 2);
	out.write(reinterpret_cast<const char*>(&ch), 2);
	out.write(reinterpret_cast<const char*>(&rate), 4);
	out.write(reinterpret_cast<const char*>(&byteRate), 4);
	out.write(reinterpret_cast<const char*>(&blockAlign), 2);
	out.write(reinterpret_cast<const char*>(&bps), 2);
	out.write("data", 4);
	out.write(reinterpret_cast<const char*>(&dataSize), 4);

	// Batch writes: ~150 KB per I/O call instead of one per sector
	constexpr size_t WRITE_BATCH = 64;
	std::vector<BYTE> buf(WRITE_BATCH * AUDIO_SECTOR_SIZE);
	size_t buffered = 0;

	for (size_t i = 0; i < sectorCount; i++) {
		size_t idx = startSector + i;
		memcpy(buf.data() + buffered * AUDIO_SECTOR_SIZE,
			sectors[idx].data(), AUDIO_SECTOR_SIZE);
		buffered++;
		if (buffered == WRITE_BATCH) {
			out.write(reinterpret_cast<const char*>(buf.data()),
				buffered * AUDIO_SECTOR_SIZE);
			buffered = 0;
		}
	}
	if (buffered > 0) {
		out.write(reinterpret_cast<const char*>(buf.data()),
			buffered * AUDIO_SECTOR_SIZE);
	}

	return out.good();
}

// Attempts WAV → FLAC conversion via flac.exe (best compression, silent).
// Returns true if the FLAC file was created successfully.
static bool ConvertWavToFlac(const std::wstring& wavPath, const std::wstring& flacPath) {
	std::wstring cmdLine = L"flac --best --silent --force -o \"" + flacPath + L"\" \"" + wavPath + L"\"";

	STARTUPINFOW si = { sizeof(si) };
	si.dwFlags = STARTF_USESHOWWINDOW;
	si.wShowWindow = SW_HIDE;
	PROCESS_INFORMATION pi = {};

	if (!CreateProcessW(nullptr, &cmdLine[0], nullptr, nullptr, FALSE,
		CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
		return false;
	}

	WaitForSingleObject(pi.hProcess, INFINITE);

	DWORD exitCode = 1;
	GetExitCodeProcess(pi.hProcess, &exitCode);
	CloseHandle(pi.hProcess);
	CloseHandle(pi.hThread);

	return exitCode == 0;
}

// Writes a track file in the requested format.
// For FLAC: writes a temp WAV, converts via flac.exe, deletes the WAV on success.
// If FLAC encoding is unavailable or fails, keeps the WAV and returns the actual path used.
static bool WriteTrackFile(TrackOutputFormat format,
	const std::wstring& basePath,        // path without extension
	const std::vector<std::vector<BYTE>>& sectors,
	size_t startSector, size_t sectorCount,
	std::wstring& actualPath,            // [out] final file path
	bool& flacFallback)                  // [out] true if fell back to WAV
{
	flacFallback = false;

	if (format == TrackOutputFormat::WAV) {
		actualPath = basePath + L".wav";
		return WriteWavFile(actualPath, sectors, startSector, sectorCount);
	}

	// FLAC: write temp WAV → convert → delete WAV
	std::wstring wavPath = basePath + L".wav";
	std::wstring flacPath = basePath + L".flac";

	if (!WriteWavFile(wavPath, sectors, startSector, sectorCount))
		return false;

	if (ConvertWavToFlac(wavPath, flacPath)) {
		DeleteFileW(wavPath.c_str());
		actualPath = flacPath;
		return true;
	}

	// flac.exe not found or failed — keep the WAV
	flacFallback = true;
	actualPath = wavPath;
	return true;
}

// ═══════════════════════════════════════════════════════════════════════════
//  Interactive menus
// ═══════════════════════════════════════════════════════════════════════════

// Track selection — returns indices into disc.tracks (audio only).
static std::vector<int> SelectTracks(const DiscInfo& disc) {
	std::vector<int> audioIdx;
	for (int i = 0; i < static_cast<int>(disc.tracks.size()); i++) {
		if (disc.tracks[i].isAudio)
			audioIdx.push_back(i);
	}
	if (audioIdx.empty()) return {};

	std::cout << "\n=== Track Selection ===\n";
	std::cout << "0. Back to menu\n";
	std::cout << "1. All audio tracks (" << audioIdx.size() << " tracks)\n";
	std::cout << "2. Select individual tracks\n";
	std::cout << "Choice: ";

	int c = GetMenuChoice(0, 2, 1);
	std::cin.clear(); std::cin.ignore(10000, '\n');

	if (c == 0) return {};
	if (c == 1) return audioIdx;

	// List tracks
	std::cout << "\nAudio tracks:\n";
	for (int idx : audioIdx) {
		const auto& t = disc.tracks[idx];
		DWORD sectors = t.endLBA - t.startLBA + 1;
		int secs = static_cast<int>(sectors / 75);
		std::cout << "  " << std::setw(2) << t.trackNumber << ". "
			<< std::setw(2) << secs / 60 << ":"
			<< std::setfill('0') << std::setw(2) << secs % 60
			<< std::setfill(' ');
		if (t.trackNumber > 0 &&
			static_cast<size_t>(t.trackNumber) <= disc.cdText.trackTitles.size() &&
			!disc.cdText.trackTitles[t.trackNumber - 1].empty()) {
			std::cout << "  " << disc.cdText.trackTitles[t.trackNumber - 1];
		}
		std::cout << "\n";
	}

	std::cout << "\nEnter track numbers separated by spaces (e.g. 1 3 5), or 0 to go back:\nTracks: ";
	std::string line;
	std::getline(std::cin, line);
	if (line.empty() || line == "0") return {};

	std::istringstream iss(line);
	std::vector<int> selected;
	int num;
	while (iss >> num) {
		for (int idx : audioIdx) {
			if (disc.tracks[idx].trackNumber == num) {
				if (std::find(selected.begin(), selected.end(), idx) == selected.end())
					selected.push_back(idx);
				break;
			}
		}
	}

	if (selected.empty()) {
		Console::Warning("No valid tracks selected.\n");
	}
	else {
		std::cout << "Selected " << selected.size() << " track(s): ";
		for (size_t i = 0; i < selected.size(); i++) {
			if (i > 0) std::cout << ", ";
			std::cout << disc.tracks[selected[i]].trackNumber;
		}
		std::cout << "\n";
	}
	return selected;
}

static int SelectOutputFormat() {
	std::cout << "\n=== Output Format ===\n";
	std::cout << "0. Back to menu\n";
	std::cout << "1. WAV (uncompressed, maximum compatibility)\n";
	std::cout << "2. FLAC (lossless compressed — requires flac.exe on PATH)\n";
	std::cout << "Choice: ";
	int c = GetMenuChoice(0, 2, 1);
	std::cin.clear(); std::cin.ignore(10000, '\n');
	if (c == 0) return -1;
	std::cout << (c == 1 ? "Output format: WAV\n" : "Output format: FLAC\n");
	return c - 1;  // 0 = WAV, 1 = FLAC
}

static int SelectRipMode(int selectedSpeed) {
	std::string speedLabel = (selectedSpeed == 0)
		? "maximum speed" : (std::to_string(selectedSpeed) + "x");
	std::cout << "\n=== Rip Mode ===\n";
	std::cout << "0. Back to menu\n";
	std::cout << "1. Safe (re-reads on error, C2-guided, recommended)\n";
	std::cout << "2. Burst (" << speedLabel << ", no secure re-reads, fastest)\n";
	std::cout << "Choice: ";
	int c = GetMenuChoice(0, 2, 1);
	std::cin.clear(); std::cin.ignore(10000, '\n');
	if (c == 0) return -1;
	std::cout << (c == 1 ? "Safe mode selected\n" : "Burst mode selected\n");
	return c;  // 1 = safe, 2 = burst
}

static int SelectVerifyMode() {
	std::cout << "\n=== Verification ===\n";
	std::cout << "0. Back to menu\n";
	std::cout << "1. None (rip only)\n";
	std::cout << "2. Verify ripped track(s) against physical disk (Test & Copy)\n";
	std::cout << "Choice: ";
	int c = GetMenuChoice(0, 2, 1);
	std::cin.clear(); std::cin.ignore(10000, '\n');
	if (c == 0) return -1;
	std::cout << (c == 1 ? "No verification selected\n" : "Verify against disk selected\n");
	return c;  // 1 = skip, 2 = verify
}

// ═══════════════════════════════════════════════════════════════════════════
//  Main workflow
// ═══════════════════════════════════════════════════════════════════════════

bool RunTrackRipWorkflow(AudioCDCopier& copier, DiscInfo& disc, const std::wstring& /*workDir*/) {
	Console::Info("\n(Enter 0 at any prompt to go back to menu)\n");

	// ── 1. Track selection ──────────────────────────────────────────────
	std::vector<int> selectedTracks = SelectTracks(disc);
	if (selectedTracks.empty()) return false;

	// Keep tracks in disc order so the drive reads sequentially (no seeks)
	std::sort(selectedTracks.begin(), selectedTracks.end());

	// ── 2. Output format ────────────────────────────────────────────────
	int fmtChoice = SelectOutputFormat();
	if (fmtChoice == -1) return false;
	TrackOutputFormat format = static_cast<TrackOutputFormat>(fmtChoice);

	// ── 3. Speed ────────────────────────────────────────────────────────
	int speed = copier.SelectSpeed();
	if (speed == -1) return false;

	// ── 4. Burst / Safe mode ────────────────────────────────────────────
	int ripMode = SelectRipMode(speed);
	if (ripMode == -1) return false;
	bool isBurst = (ripMode == 2);

	// ── 5. Verification Mode ────────────────────────────────────────────
	int verifyMode = SelectVerifyMode();
	if (verifyMode == -1) return false;
	bool verifyRip = (verifyMode == 2);

	// ── 6. Drive capabilities ───────────────────────────────────────────
	SecureRipConfig secureConfig{};
	if (!isBurst) {
		std::cout << "\nDetecting drive capabilities..." << std::flush;
		DriveCapabilities caps;
		if (copier.DetectDriveCapabilities(caps)) {
			disc.enableC2Detection = caps.supportsC2ErrorReporting;
			secureConfig = copier.GetSecureRipConfig(SecureRipMode::Standard);
			secureConfig.useC2 = caps.supportsC2ErrorReporting;
			secureConfig.c2Guided = caps.supportsC2ErrorReporting;
			if (caps.supportsAccurateStream) {
				secureConfig.cacheDefeat = false;
				Console::Info(" Accurate Stream detected.\n");
			}
			else {
				secureConfig.cacheDefeat = true;
				std::cout << " done.\n";
			}
		}
		else {
			disc.enableC2Detection = false;
			std::cout << " skipped.\n";
		}
	}
	else {
		disc.enableC2Detection = false;
	}

	// ── 7. Offset correction ────────────────────────────────────────────
	int offset = copier.SelectOffset();
	if (offset == -1) return false;
	disc.driveOffset = offset;

	// ── 8. Output directory ─────────────────────────────────────────────
	std::wstring outputDir;
	while (true) {
		std::cout << "\nOutput directory (or 0 to go back):\n";
		Console::SetColor(Console::Color::DarkGray);
		std::cout << "  Examples: C:\\Music\\  or  .\\rips\\\n";
		Console::Reset();
		std::cout << "Path: ";

		std::string narrowPath;
		std::getline(std::cin, narrowPath);

		if (narrowPath == "0") return false;

		if (narrowPath.empty()) {
			outputDir.clear();
		}
		else if (!Utf8ToWide(narrowPath, outputDir)) {
			Console::Error("Invalid UTF-8 path. Please try again.\n");
			continue;
		}

		outputDir = NormalizePath(outputDir);

		if (outputDir.empty()) {
			Console::Error("Empty path. Please try again.\n");
			continue;
		}
		if (!outputDir.empty() && outputDir.back() != L'\\' && outputDir.back() != L'/') {
			outputDir += L"\\";
		}
		if (!CreateDirectoryRecursive(outputDir)) {
			Console::Error("Cannot create directory.\n");
			continue;
		}

		Console::Success("Output directory validated.\n");
		break;
	}

	// ── 9. Configure disc state & build trimmed copy ────────────────────
	disc.pregapMode = PregapMode::Skip;
	disc.includeSubchannel = false;
	disc.enableCacheDefeat = !isBurst && secureConfig.cacheDefeat;

	if (disc.sessionCount > 1) {
		Console::Info("Multi-session disc — using session 1 (audio).\n");
	}

	// When offset correction is active and the user picked non-contiguous
	// tracks (e.g. 1, 5, 10), ApplyOffsetCorrection would shift samples
	// across the artificial gaps.  Fill those gaps so the sector stream
	// reaching the offset corrector is truly contiguous on disc.
	int firstIdx = selectedTracks.front();
	int lastIdx = selectedTracks.back();
	bool isContiguous = (lastIdx - firstIdx + 1 == static_cast<int>(selectedTracks.size()));
	bool needGapFill = (offset != 0 && !isContiguous);

	DiscInfo ripDisc = disc;
	ripDisc.rawSectors.clear();
	ripDisc.tracks.clear();

	// ripIndices[i] = index into ripDisc.tracks for selectedTracks[i]
	std::vector<int> ripIndices(selectedTracks.size());

	if (needGapFill) {
		// Include every track from first to last selected
		for (int i = firstIdx; i <= lastIdx; i++) {
			ripDisc.tracks.push_back(disc.tracks[i]);
		}
		for (size_t i = 0; i < selectedTracks.size(); i++) {
			ripIndices[i] = selectedTracks[i] - firstIdx;
		}
	}
	else {
		for (int idx : selectedTracks) {
			ripDisc.tracks.push_back(disc.tracks[idx]);
		}
		for (size_t i = 0; i < selectedTracks.size(); i++) {
			ripIndices[i] = static_cast<int>(i);
		}
	}
	ripDisc.selectedSession = 0;   // not needed — we already picked the tracks

	// ── 10. Read only the selected tracks ───────────────────────────────
	Console::Info("\nReading disc...\n");
	ProgressIndicator prog;
	prog.SetLabel("  Ripping");
	prog.Start();

	bool readOk = false;
	SecureRipResult secureResult;

	if (isBurst) {
		readOk = copier.ReadDiscBurst(ripDisc, MakeProgressCallback(&prog), speed);
	}
	else {
		readOk = copier.ReadDiscSecure(ripDisc, secureConfig, secureResult,
			MakeProgressCallback(&prog));
	}

	if (!readOk) {
		prog.Finish(false);
		Console::Error("Disc read failed.\n");
		return false;
	}
	prog.Finish(true);

	if (isBurst && ripDisc.errorCount > 0) {
		std::string msg = std::to_string(ripDisc.errorCount) +
			" sector read error(s) occurred in burst mode; affected output may contain unreadable or zero-filled audio.\n";
		Console::Warning(msg.c_str());
		if (!verifyRip) {
			Console::Info("Consider Safe mode or enabling physical compare for a second-pass check.\n");
		}
	}

	if (!isBurst && secureResult.unsecureSectors > 0) {
		std::string msg = std::to_string(secureResult.unsecureSectors) +
			" sector(s) could not be fully verified.\n";
		Console::Warning(msg.c_str());
	}

	// Apply offset correction before splitting tracks
	if (ripDisc.driveOffset != 0) {
		copier.ApplyOffsetCorrection(ripDisc);
	}

	// ── 11. Build per-track sector map ──────────────────────────────────
	// ripDisc.tracks may include gap-fill tracks; slices covers all of them.
	struct TrackSlice { size_t start; size_t count; };
	std::vector<TrackSlice> slices(ripDisc.tracks.size());
	size_t cumIdx = 0;
	for (size_t i = 0; i < ripDisc.tracks.size(); i++) {
		DWORD readStart = (ripDisc.pregapMode == PregapMode::Skip)
			? ripDisc.tracks[i].startLBA : ripDisc.tracks[i].pregapLBA;
		DWORD cnt = ripDisc.tracks[i].endLBA - readStart + 1;
		slices[i] = { cumIdx, cnt };
		cumIdx += cnt;
	}

	// ── 12. Save each selected track ────────────────────────────────────
	Console::Info("\nSaving tracks...\n");
	int savedCount = 0;
	bool anyFlacFallback = false;

	for (size_t si = 0; si < selectedTracks.size(); si++) {
		int ri = ripIndices[si];
		const auto& t = ripDisc.tracks[ri];
		const TrackSlice& sl = slices[ri];

		// Build filename: "02. Artist - Title" or "Track 02"
		std::wostringstream prefix;
		prefix << std::setfill(L'0') << std::setw(2) << t.trackNumber << L". ";

		std::wstring baseName;
		bool hasCDText = (t.trackNumber > 0 &&
			static_cast<size_t>(t.trackNumber) <= disc.cdText.trackTitles.size() &&
			!disc.cdText.trackTitles[t.trackNumber - 1].empty());

		if (hasCDText) {
			std::string title = disc.cdText.trackTitles[t.trackNumber - 1];
			std::string artist;
			if (t.trackNumber > 0 &&
				static_cast<size_t>(t.trackNumber) <= disc.cdText.trackArtists.size() &&
				!disc.cdText.trackArtists[t.trackNumber - 1].empty()) {
				artist = disc.cdText.trackArtists[t.trackNumber - 1];
			}
			std::string narrow = artist.empty() ? title : (artist + " - " + title);

			std::wstring wide;
			if (Utf8ToWide(narrow, wide)) {
				std::wstring sanitized = SanitizeFilename(wide);
				if (!sanitized.empty()) {
					baseName = prefix.str() + sanitized;
				}
			}
		}

		if (baseName.empty()) {
			baseName = L"Track " + prefix.str().substr(0, 2);
		}

		std::wstring basePath = outputDir + baseName;
		std::wstring actualPath;
		bool flacFallback = false;

		bool ok = WriteTrackFile(format, basePath, ripDisc.rawSectors,
			sl.start, sl.count, actualPath, flacFallback);

		if (flacFallback) anyFlacFallback = true;

		if (ok) {
			Console::Success("  Saved: ");
			std::wcout << baseName;
			if (flacFallback)
				std::cout << " (WAV — FLAC encoding unavailable)";
			std::cout << "\n";
			savedCount++;
		}
		else {
			Console::Error("  Failed: ");
			std::wcout << baseName << L"\n";
		}
	}

	if (anyFlacFallback) {
		Console::Warning("\nFLAC encoding unavailable or failed — affected tracks were saved as WAV.\n");
		Console::Info("Install FLAC command-line tools (https://xiph.org/flac/) and ensure\n");
		Console::Info("flac.exe is on your system PATH. If it is already installed, verify it launches correctly.\n");
	}

	std::string summary = "\n" + std::to_string(savedCount) + "/" +
		std::to_string(selectedTracks.size()) + " track(s) saved to: ";
	
	if (savedCount == 0) {
		Console::Error(summary.c_str());
	}
	else if (savedCount < static_cast<int>(selectedTracks.size())) {
		Console::Warning(summary.c_str());
	}
	else {
		Console::Success(summary.c_str());
	}
	std::wcout << outputDir << L"\n";

	if (savedCount == 0) {
		ripDisc.rawSectors.clear();
		ripDisc.rawSectors.shrink_to_fit();
		std::cout << "\n";
		return false;
	}

	// ── 13. Verify ripped tracks against physical disk ──────────────────
	if (verifyRip) {
		Console::Info("\nVerifying against physical disk (second pass)...\n");
		ProgressIndicator verProg;
		verProg.SetLabel("  Verifying");
		verProg.Start();

		DiscInfo verifyDisc = ripDisc;
		verifyDisc.rawSectors.clear();
		// Force cache defeat to ensure the drive actually re-reads the physical disc surface
		verifyDisc.enableCacheDefeat = true;

		bool verifyReadOk = false;
		SecureRipResult vResult;

		if (isBurst) {
			verifyReadOk = copier.ReadDiscBurst(verifyDisc, MakeProgressCallback(&verProg), speed);
		}
		else {
			verifyReadOk = copier.ReadDiscSecure(verifyDisc, secureConfig, vResult,
				MakeProgressCallback(&verProg));
		}

		if (!verifyReadOk) {
			verProg.Finish(false);
			Console::Error("Verification read failed.\n");
		}
		else {
			verProg.Finish(true);

			if (isBurst && verifyDisc.errorCount > 0) {
				std::string msg = std::to_string(verifyDisc.errorCount) +
					" sector read error(s) occurred during the verification pass.\n";
				Console::Warning(msg.c_str());
			}

			if (verifyDisc.driveOffset != 0) {
				copier.ApplyOffsetCorrection(verifyDisc);
			}

			bool allMatch = true;
			std::cout << "\n";
			for (size_t si = 0; si < selectedTracks.size(); si++) {
				int ri = ripIndices[si];
				const auto& t = verifyDisc.tracks[ri];
				const TrackSlice& sl = slices[ri];

				std::cout << "  Track " << std::setw(2) << t.trackNumber << ": ";

				if (TrackHadReadErrors(ripDisc, ripDisc.tracks[ri]) ||
					TrackHadReadErrors(verifyDisc, verifyDisc.tracks[ri])) {
					Console::Warning("[READ ERRORS]\n");
					allMatch = false;
					continue;
				}

				bool trackMatch = true;
				for (size_t i = 0; i < sl.count; i++) {
					size_t idx = sl.start + i;
					if (idx >= ripDisc.rawSectors.size() || idx >= verifyDisc.rawSectors.size()) {
						trackMatch = false;
						break;
					}
					if (memcmp(ripDisc.rawSectors[idx].data(), verifyDisc.rawSectors[idx].data(), AUDIO_SECTOR_SIZE) != 0) {
						trackMatch = false;
						break;
					}
				}

				if (trackMatch) {
					Console::Success("[MATCH]\n");
				}
				else {
					Console::Error("[MISMATCH]\n");
					allMatch = false;
				}
			}

			if (allMatch) {
				Console::Success("\nAll ripped tracks verified successfully against the disk.\n");
			}
			else {
				Console::Warning("\nSome tracks did not match. Check disc condition or drive caching.\n");
			}
		}
		verifyDisc.rawSectors.clear();
	}

	// ── 14. Cleanup ─────────────────────────────────────────────────────
	ripDisc.rawSectors.clear();
	ripDisc.rawSectors.shrink_to_fit();

	std::cout << "\n";
	return true;
}