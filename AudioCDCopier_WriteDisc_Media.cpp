#define NOMINMAX
#include "AudioCDCopier.h"
#include "ConsoleColors.h"
#include "InterruptHandler.h"
#include <chrono>
#include <conio.h>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <windows.h>

// ============================================================================
// Helper: Wait for drive to become ready (poll TEST UNIT READY)
// ============================================================================
static bool WaitForDriveReady(ScsiDrive& drive, int timeoutSeconds) {
	for (int i = 0; i < timeoutSeconds * 4; i++) {
		BYTE testCmd[6] = { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };
		BYTE sk = 0, asc = 0, ascq = 0;
		if (drive.SendSCSIWithSense(testCmd, sizeof(testCmd), nullptr, 0,
			&sk, &asc, &ascq, true)) {
			return true;
		}
		if (sk == 0x02 && asc == 0x04) {
			Sleep(250);
			continue;
		}
		return false;
	}
	return false;
}

// ============================================================================
// CheckRewritableDisk - Detect rewritable disc and capacity
// ============================================================================
bool AudioCDCopier::CheckRewritableDisk(bool& isFull, bool& isRewritable) {
	isFull = false;
	isRewritable = false;

	Console::Info("Querying disc media type...\n");

	BYTE cmd[10] = { 0x51, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xFC, 0x00 };
	BYTE response[252] = { 0 };
	BYTE senseKey = 0, asc = 0, ascq = 0;

	if (!m_drive.SendSCSIWithSense(cmd, sizeof(cmd), response, sizeof(response),
		&senseKey, &asc, &ascq, true)) {

		Console::Warning("Disc information query failed");
		std::string senseDesc = m_drive.GetSenseDescription(senseKey, asc, ascq);
		if (!senseDesc.empty()) {
			Console::Info(" (");
			std::cout << senseDesc << ")\n";
		}
		else {
			std::cout << "\n";
		}

		Console::Warning("Attempting fallback disc detection...\n");

		BYTE profileCmd[10] = { 0x46, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x08, 0x00 };
		BYTE profileResponse[8] = { 0 };

		if (m_drive.SendSCSI(profileCmd, sizeof(profileCmd), profileResponse, sizeof(profileResponse), true)) {
			WORD profile = (static_cast<WORD>(profileResponse[6]) << 8) | profileResponse[7];
			isRewritable = (profile == 0x0A);

			if (isRewritable)
				isFull = true;

			Console::Success("Media type detected: ");
			switch (profile) {
			case 0x08: std::cout << "CD-ROM\n"; break;
			case 0x09: std::cout << "CD-R (write-once)\n"; isRewritable = false; break;
			case 0x0A: std::cout << "CD-RW (rewritable)\n"; isRewritable = true; break;
			default:
				std::cout << "Unknown (0x" << std::hex << profile << std::dec << ")\n";
				break;
			}
			return true;
		}

		Console::Warning("Could not determine disc type\n");
		return false;
	}

	BYTE discStatus = response[2] & 0x03;
	isFull = (discStatus == 0x02);
	isRewritable = (response[2] & 0x10) != 0;

	Console::Success("Disc media type: ");
	std::cout << (isRewritable ? "CD-RW (rewritable)\n" : "CD-R (write-once)\n");

	Console::Success("Disc status: ");
	switch (discStatus) {
	case 0x00: std::cout << "Empty\n"; break;
	case 0x01: std::cout << "Appendable\n"; break;
	case 0x02: std::cout << "Complete (full)\n"; break;
	default: std::cout << "Unknown\n"; break;
	}

	return true;
}

// ============================================================================
// BlankRewritableDisk - Erase rewritable media (quick or full)
// ============================================================================
bool AudioCDCopier::BlankRewritableDisk(int speed, bool quickBlank) {
	Console::Warning(quickBlank ? "\nQuick blanking rewritable disc...\n"
		: "\nFull blanking rewritable disc...\n");
	Console::Info("⚠ This operation will erase all data on the disc!\n");
	std::cout << "Confirm blanking? (y/n): ";
	char confirm = _getch();
	std::cout << confirm << "\n";
	if (tolower(confirm) != 'y') {
		Console::Info("Blank operation cancelled\n");
		return false;
	}

	m_drive.SetSpeed(speed);

	BYTE standardType = (quickBlank ? 0x01 : 0x00) | 0x10;
	BYTE cmd[12] = { 0xA1, standardType, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };

	bool blankStarted = false;
	const char* usedMethod = quickBlank ? "quick blank" : "full blank";

	if (m_drive.SendSCSI(cmd, sizeof(cmd), nullptr, 0, false)) {
		blankStarted = true;
	}
	else {
		Console::Warning("Standard blank failed — trying erase session recovery...\n");

		BYTE recoveryCmd[12] = { 0xA1, 0x16, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };

		if (m_drive.SendSCSI(recoveryCmd, sizeof(recoveryCmd), nullptr, 0, false)) {
			Console::Info("Erase session in progress...\n");
			WaitForDriveReady(m_drive, 120);

			Console::Info("Retrying ");
			std::cout << usedMethod << "...\n";
			if (m_drive.SendSCSI(cmd, sizeof(cmd), nullptr, 0, false)) {
				blankStarted = true;
			}
			else {
				Console::Error("Retry after erase session also failed\n");
			}
		}
		else {
			Console::Warning("Erase session not supported by drive\n");
		}
	}

	if (!blankStarted) {
		Console::Error("Blank command failed\n");
		return false;
	}

	Console::Info("Blanking method: ");
	std::cout << usedMethod << "\n";

	int maxWait = quickBlank ? 120 : 600;
	int barWidth = 35;
	std::string label = quickBlank ? "Quick blank" : "Full blank";
	int lastPct = -1;
	int lastLineLen = 0;
	auto startTime = std::chrono::steady_clock::now();

	for (int i = 0; i < maxWait; i++) {
		Sleep(1000);

		if (InterruptHandler::Instance().IsInterrupted()) {
			std::cout << "\n";
			Console::Error("Blank operation cancelled\n");
			return false;
		}

		BYTE sk = 0, asc = 0, ascq = 0;
		int drivePct = -1;
		m_drive.RequestSenseProgress(sk, asc, ascq, drivePct);

		BYTE testCmd[6] = { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };
		BYTE tsk = 0, tasc = 0, tascq = 0;
		if (m_drive.SendSCSIWithSense(testCmd, sizeof(testCmd), nullptr, 0,
			&tsk, &tasc, &tascq, true)) {
			drivePct = 100;
		}

		int pct;
		if (drivePct >= 0) {
			pct = std::min(drivePct, 100);
		}
		else {
			pct = std::min((i + 1) * 100 / maxWait, 99);
		}

		if (pct != lastPct) {
			lastPct = pct;
			std::ostringstream ss;
			ss << "\r" << label << " [";
			int filled = pct * barWidth / 100;
			for (int j = 0; j < barWidth; j++) {
				ss << (j < filled ? "\xe2\x96\x88" : "\xe2\x96\x91");
			}
			ss << "] " << std::setw(3) << pct << "%";

			auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
				std::chrono::steady_clock::now() - startTime).count();
			if (elapsed >= 60)
				ss << " " << elapsed / 60 << "m " << std::setfill('0') << std::setw(2) << elapsed % 60 << "s";
			else
				ss << " " << elapsed << "s";

			std::string line = ss.str();
			if (static_cast<int>(line.size()) < lastLineLen)
				line.append(lastLineLen - line.size(), ' ');
			std::cout << line << std::flush;
			lastLineLen = static_cast<int>(line.size());
		}

		if (drivePct >= 100) break;
	}

	auto totalSec = std::chrono::duration_cast<std::chrono::seconds>(
		std::chrono::steady_clock::now() - startTime).count();
	std::cout << "\n  Done";
	if (totalSec > 0) {
		if (totalSec >= 60)
			std::cout << " in " << totalSec / 60 << "m "
			<< std::setfill('0') << std::setw(2) << totalSec % 60 << "s";
		else
			std::cout << " in " << totalSec << "s";
	}
	std::cout << "\n";

	Console::Success("Disc blanked successfully\n");
	return true;
}

// ============================================================================
// PerformPowerCalibration - Calibrate laser power for writing
// ============================================================================
bool AudioCDCopier::PerformPowerCalibration() {
	Console::Info("Performing power calibration (OPC)...\n");

	BYTE cmd[10] = { 0x54, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };

	if (!m_drive.SendSCSI(cmd, sizeof(cmd), nullptr, 0, false)) {
		Console::Warning("Power calibration not supported by drive (continuing)\n");
		return true;
	}

	Console::Info("Waiting for power calibration to complete");
	for (int i = 0; i < 5; i++) {
		std::cout << ".";
		Sleep(1000);
		if (InterruptHandler::Instance().IsInterrupted()) {
			Console::Error("\nPower calibration cancelled\n");
			return false;
		}
	}
	std::cout << "\n";

	Console::Success("Power calibration complete\n");
	return true;
}