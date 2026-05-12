#pragma once
#include "AudioCDCopier.h"
#include <string>

int RunMainMenuLoop(AudioCDCopier& copier, DiscInfo& disc, const std::wstring& workDir, wchar_t& audioDrive, bool& hasTOC);