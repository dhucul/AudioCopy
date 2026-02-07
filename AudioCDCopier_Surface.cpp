#define NOMINMAX
#include "AudioCDCopier.h"
#include "InterruptHandler.h"
#include <iostream>
#include <fstream>
#include <iomanip>
#include <vector>

// ============================================================================
// Surface Mapping & Lead Area Checks
// ============================================================================

bool AudioCDCopier::CheckLeadAreas(DiscInfo& disc, int scanSpeed) {
	std::cout << "\n=== Lead-in/Lead-out Area Check ===\n";
	if (!m_drive.CheckC2Support()) return false;
	m_drive.SetSpeed(scanSpeed);

	int leadInErrors = 0, leadOutErrors = 0;
	for (DWORD lba = 0; lba < 150; lba++) {
		std::vector<BYTE> buf(SECTOR_WITH_C2_SIZE);
		int c2 = 0;
		if (m_drive.ReadSectorWithC2(lba, buf.data(), nullptr, c2) && c2 > 0) leadInErrors++;
	}

	DWORD leadOutStart = disc.leadOutLBA > 150 ? disc.leadOutLBA - 150 : 0;
	for (DWORD lba = leadOutStart; lba < disc.leadOutLBA; lba++) {
		std::vector<BYTE> buf(SECTOR_WITH_C2_SIZE);
		int c2 = 0;
		if (m_drive.ReadSectorWithC2(lba, buf.data(), nullptr, c2) && c2 > 0) leadOutErrors++;
	}
	m_drive.SetSpeed(0);
	std::cout << "Lead-in errors: " << leadInErrors << ", Lead-out errors: " << leadOutErrors << "\n";
	return (leadInErrors == 0 && leadOutErrors == 0);
}

void AudioCDCopier::GenerateSurfaceMap(DiscInfo& disc, const std::wstring& filename, int scanSpeed) {
	std::cout << "\n=== Generating Disc Surface Map ===\n";
	m_drive.SetSpeed(scanSpeed);

	DWORD totalSectors = CalculateTotalAudioSectors(disc);

	if (totalSectors == 0) {
		std::cout << "No audio tracks to scan.\n";
		return;
	}

	std::ofstream mapFile;
	if (!filename.empty()) {
		mapFile.open(filename);
		if (!mapFile) {
			std::cout << "ERROR: Cannot create map file.\n";
			return;
		}
		mapFile << "LBA,C2_Errors,Status\n";
	}

	ProgressIndicator progress(40);
	progress.SetLabel("  Surface Scan");
	progress.Start();

	std::vector<BYTE> buf(SECTOR_WITH_C2_SIZE);
	DWORD scannedSectors = 0;
	int totalC2Errors = 0;
	int failedReads = 0;
	int errorSectors = 0;

	for (const auto& t : disc.tracks) {
		if (!t.isAudio) continue;
		DWORD start = (t.trackNumber == 1) ? 0 : t.pregapLBA;

		for (DWORD lba = start; lba <= t.endLBA; lba++) {
			if (g_interrupt.IsInterrupted() || g_interrupt.CheckEscapeKey()) {
				m_drive.SetSpeed(0);
				progress.Finish(false);
				return;
			}

			int c2Errors = 0;
			bool readOk = m_drive.ReadSectorWithC2(lba, buf.data(), nullptr, c2Errors);

			if (!readOk) failedReads++;
			if (c2Errors > 0) { errorSectors++; totalC2Errors += c2Errors; }

			if (mapFile.is_open()) {
				mapFile << lba << "," << c2Errors << ","
					<< (readOk ? "OK" : "FAIL") << "\n";
			}

			scannedSectors++;
			progress.Update(static_cast<int>(scannedSectors), static_cast<int>(totalSectors));
		}
	}

	progress.Finish(true);
	m_drive.SetSpeed(0);

	std::cout << "\n--- Surface Map Summary ---\n";
	std::cout << "  Sectors scanned:  " << scannedSectors << "\n";
	std::cout << "  C2 error sectors: " << errorSectors << "\n";
	std::cout << "  Total C2 errors:  " << totalC2Errors << "\n";
	std::cout << "  Read failures:    " << failedReads << "\n";

	if (!filename.empty()) {
		std::wcout << L"  Map saved: " << filename << L"\n";
	}
}