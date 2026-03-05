#pragma once
#include <windows.h>
#include <vector>
#include <string>

// Drive operation constants
constexpr int DRIVE_POLL_INTERVAL_MS = 500;
constexpr DWORD AUDIO_TRACK_MASK = 0x04;

// Drive information and operations
HANDLE OpenDriveHandle(wchar_t letter);
std::string GetDriveName(HANDLE h);
int GetAudioTrackCount(HANDLE h);
bool WaitForMediaReady(HANDLE h, int maxWaitMs = 5000);
bool CheckForAudioTracks(HANDLE h);
std::vector<wchar_t> ScanDrives(std::vector<wchar_t>& audioDrives);
wchar_t WaitForDisc(const std::vector<wchar_t>& cdDrives, int timeoutSeconds = 0);
std::string GetDiscStatus(HANDLE h, bool& hasAudio, int& audioTracks);