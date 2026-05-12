// ============================================================================
// WriteTracksWorkflow.h - Write ripped track files to disc using the
// pregap layout of the disc currently in the drive.
// ============================================================================
#pragma once

#include "AudioCDCopier.h"
#include <string>

// Builds a temporary .bin/.cue from the WAV/FLAC files in a folder, using
// pregap durations from the source disc's TOC, then writes a new disc.
//
// The source disc must be inserted when the workflow starts (its TOC is read
// from `disc`); the user is prompted to swap to a blank disc before burning.
void RunWriteTracksWorkflow(AudioCDCopier& copier, DiscInfo& disc,
    const std::wstring& workDir, wchar_t audioDrive);
