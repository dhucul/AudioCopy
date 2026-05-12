#pragma once
#include "AudioCDCopier.h"
#include <string>
#include <vector>

bool RunCopyWorkflow(AudioCDCopier& copier, DiscInfo& disc, const std::wstring& workDir);
void RunWriteDiscWorkflow(AudioCDCopier& copier, const std::wstring& workDir);