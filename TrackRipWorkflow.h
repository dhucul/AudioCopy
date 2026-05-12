// ============================================================================
// TrackRipWorkflow.h - Rip individual tracks to WAV or FLAC
// ============================================================================
#pragma once

#include "AudioCDCopier.h"
#include <string>

enum class TrackOutputFormat { WAV = 0, FLAC = 1 };

// Runs the interactive track-rip workflow: track selection, format, speed,
// burst/safe mode, ripping with progress, and AccurateRip CRC verification.
bool RunTrackRipWorkflow(AudioCDCopier& copier, DiscInfo& disc, const std::wstring& workDir);