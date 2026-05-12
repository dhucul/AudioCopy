#include "DriveSelection.h"
#include "ConsoleColors.h"
#include "Drive.h"
#include "MenuHelpers.h"
#include <iostream>

wchar_t SelectAudioDrive(const std::vector<wchar_t>& audioDrives) {
	Console::Warning("\nMultiple audio CDs detected. Select drive:\n");
	for (size_t i = 0; i < audioDrives.size(); i++) {
		HANDLE h = OpenDriveHandle(audioDrives[i]);
		std::string name = (h != INVALID_HANDLE_VALUE) ? GetDriveName(h) : "CD/DVD drive";
		int tracks = (h != INVALID_HANDLE_VALUE) ? GetAudioTrackCount(h) : 0;
		if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
		std::cout << "  " << (i + 1) << ". [";
		Console::SetColor(Console::Color::Yellow);
		std::cout << static_cast<char>(audioDrives[i]) << ":";
		Console::Reset();
		std::cout << "] " << name;
		if (tracks > 0) std::cout << " (" << tracks << " tracks)";
		std::cout << "\n";
	}
	std::cout << "Choice: ";
	int pick = GetMenuChoice(1, static_cast<int>(audioDrives.size()), 1);
	std::cin.clear();
	if (std::cin.peek() == '\n') std::cin.ignore();
	return audioDrives[pick - 1];
}