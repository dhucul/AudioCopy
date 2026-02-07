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

	std::cout << "\n--- Lead Area Results ---\n";
	std::cout << "  Lead-in  (first 150 sectors):  " << leadInErrors << " errors";
	if (leadInErrors == 0) std::cout << "  [OK]";
	else std::cout << "  [WARN - TOC area may be degraded]";
	std::cout << "\n";
	std::cout << "  Lead-out (last 150 sectors):   " << leadOutErrors << " errors";
	if (leadOutErrors == 0) std::cout << "  [OK]";
	else std::cout << "  [WARN - outer edge damage]";
	std::cout << "\n";

	if (leadInErrors > 0 || leadOutErrors > 0) {
		std::cout << "\n  Note: Lead area errors can cause disc recognition problems\n";
		std::cout << "        and indicate edge-region physical damage.\n";
	}
	else {
		std::cout << "\n  Lead areas are intact. Disc structure is healthy.\n";
	}

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

	std::cout << "\n" << std::string(60, '=') << "\n";
	std::cout << "              SURFACE MAP SUMMARY\n";
	std::cout << std::string(60, '=') << "\n";
	std::cout << "  Sectors scanned:  " << scannedSectors << "\n";
	std::cout << "  C2 error sectors: " << errorSectors;
	if (scannedSectors > 0)
		std::cout << " (" << std::fixed << std::setprecision(3)
		<< (errorSectors * 100.0 / scannedSectors) << "%)";
	std::cout << "\n";
	std::cout << "  Total C2 errors:  " << totalC2Errors << "\n";
	std::cout << "  Read failures:    " << failedReads << "\n";

	std::cout << "\n  Surface condition: ";
	if (failedReads > 0)
		std::cout << "DAMAGED - unreadable sectors present\n";
	else if (errorSectors == 0)
		std::cout << "CLEAN - no errors detected\n";
	else if (errorSectors * 100.0 / scannedSectors < 0.01)
		std::cout << "GOOD - very minor surface wear\n";
	else if (errorSectors * 100.0 / scannedSectors < 0.1)
		std::cout << "FAIR - some surface damage\n";
	else
		std::cout << "POOR - significant surface damage\n";

	if (!filename.empty()) {
		std::wcout << L"  Map saved to:     " << filename << L"\n";
		std::cout << "  (Open CSV in a spreadsheet to visualize error positions)\n";
	}
	std::cout << std::string(60, '=') << "\n";
}