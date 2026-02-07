#pragma once
#include "AudioCDCopier.h"
#include <string>

bool RunCopyWorkflow(AudioCDCopier& copier, DiscInfo& disc, const std::wstring& workDir);