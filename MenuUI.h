#pragma once
#include "AudioCDCopier.h"

void CenterConsoleWindow();
void WaitForKey(const char* message = "\nPress any key to return to menu...\n");
void PrintMenuItem(int num, const char* text, bool dimmed = false);
void PrintMenuSection(const char* label);
void PrintHelpMenu();