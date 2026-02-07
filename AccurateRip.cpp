// ============================================================================
// AccurateRip.cpp - AccurateRip implementation
// ============================================================================
#include "AccurateRip.h"
#include <winhttp.h>
#include <iostream>
#include <iomanip>

#pragma comment(lib, "winhttp.lib")

uint32_t AccurateRip::CalculateCRC(const std::vector<std::vector<BYTE>>& sectors,
    int trackNum, int totalTracks, DWORD trackStartLBA) {
    uint32_t crc = 0;
    uint32_t mult = 1;  // Always start at 1 for each track
    
    // Calculate samples per sector (588 samples = 2352 bytes / 4)
    const int SAMPLES_PER_SECTOR = AUDIO_SECTOR_SIZE / 4;
    
    // AccurateRip V1: Skip first/last 5 sectors (2940 samples) of DISC
    // For first track: skip first 5 sectors
    // For last track: skip last 5 sectors
    size_t skipStartSectors = (trackNum == 1) ? 5 : 0;
    size_t skipEndSectors = (trackNum == totalTracks) ? 5 : 0;
    
    for (size_t i = 0; i < sectors.size(); i++) {
        const BYTE* data = sectors[i].data();
        
        for (int j = 0; j < AUDIO_SECTOR_SIZE; j += 4) {
            uint32_t sample = static_cast<uint32_t>(data[j]) |
                (static_cast<uint32_t>(data[j + 1]) << 8) |
                (static_cast<uint32_t>(data[j + 2]) << 16) |
                (static_cast<uint32_t>(data[j + 3]) << 24);
            
            // Skip first 5 sectors of first track, last 5 sectors of last track
            bool skip = (trackNum == 1 && i < skipStartSectors) ||
                        (trackNum == totalTracks && i >= sectors.size() - skipEndSectors);
            
            if (!skip) {
                crc += sample * mult;
            }
            mult++;
        }
    }
    return crc;
}

uint32_t AccurateRip::CalculateDiscID1(const DiscInfo& disc) {
    uint32_t id = 0;
    for (const auto& t : disc.tracks) id += t.startLBA;
    id += disc.leadOutLBA;
    return id;
}

uint32_t AccurateRip::CalculateDiscID2(const DiscInfo& disc) {
    uint32_t id = 0;
    for (const auto& t : disc.tracks) {
        uint32_t frameNum = (t.startLBA == 0) ? 1 : t.startLBA;
        id += frameNum * t.trackNumber;
    }
    id += disc.leadOutLBA * (static_cast<uint32_t>(disc.tracks.size()) + 1);
    return id;
}

uint32_t AccurateRip::CalculateCDDBID(const DiscInfo& disc) {
    auto sumDigits = [](int n) {
        int sum = 0;
        while (n > 0) { sum += n % 10; n /= 10; }
        return sum;
    };

    uint32_t n = 0;
    for (const auto& t : disc.tracks) {
        int seconds = (t.startLBA + 150) / 75;
        n += sumDigits(seconds);
    }

    int leadOutSec = (disc.leadOutLBA + 150) / 75;
    int firstTrackSec = (disc.tracks.front().startLBA + 150) / 75;
    int totalSec = leadOutSec - firstTrackSec;

    return ((n % 0xFF) << 24) | (totalSec << 8) | static_cast<uint32_t>(disc.tracks.size());
}

// Add this helper function before Lookup()
static bool IsInternetAvailable() {
    HINTERNET hSession = WinHttpOpen(L"ConnectionTest",
        WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, nullptr, nullptr, 0);
    if (!hSession) return false;
    
    HINTERNET hConnect = WinHttpConnect(hSession, L"www.accuraterip.com",
        INTERNET_DEFAULT_HTTP_PORT, 0);
    
    bool available = (hConnect != nullptr);
    
    if (hConnect) WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);
    return available;
}

bool AccurateRip::Lookup(DiscInfo& disc) {
    if (!IsInternetAvailable()) {
        std::cout << "  SKIPPED: No internet connection available\n";
        return false;
    }
    
    uint32_t discId1 = CalculateDiscID1(disc);
    uint32_t discId2 = CalculateDiscID2(disc);
    uint32_t cddbId = CalculateCDDBID(disc);
    int trackCount = static_cast<int>(disc.tracks.size());

    char url[256];
    snprintf(url, sizeof(url),
        "/accuraterip/%x/%x/%x/dBAR-%03d-%08x-%08x-%08x.bin",
        discId1 & 0xF, (discId1 >> 4) & 0xF, (discId1 >> 8) & 0xF,
        trackCount, discId1, discId2, cddbId);

    std::cout << "AccurateRip lookup...\n";
    std::cout << "  Disc ID 1: " << std::hex << std::setfill('0') 
              << std::setw(8) << discId1 << std::dec << "\n";
    std::cout << "  Disc ID 2: " << std::hex << std::setfill('0') 
              << std::setw(8) << discId2 << std::dec << "\n";
    std::cout << "  CDDB ID:   " << std::hex << std::setfill('0') 
              << std::setw(8) << cddbId << std::dec << "\n";

    HINTERNET hSession = WinHttpOpen(L"AudioCopy/1.0",
        WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, nullptr, nullptr, 0);
    if (!hSession) {
        std::cout << "  SKIPPED: Failed to initialize HTTP\n";
        return false;
    }

    WinHttpSetTimeouts(hSession, 5000, 10000, 5000, 10000);

    HINTERNET hConnect = WinHttpConnect(hSession, L"www.accuraterip.com",
        INTERNET_DEFAULT_HTTP_PORT, 0);
    if (!hConnect) {
        WinHttpCloseHandle(hSession);
        std::cout << "  SKIPPED: Cannot connect to AccurateRip\n";
        return false;
    }

    wchar_t wUrl[256];
    MultiByteToWideChar(CP_UTF8, 0, url, -1, wUrl, 256);

    HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"GET", wUrl, nullptr,
        WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, 0);
    
    bool found = false;
    if (hRequest && WinHttpSendRequest(hRequest, nullptr, 0, nullptr, 0, 0, 0) &&
        WinHttpReceiveResponse(hRequest, nullptr)) {
        
        DWORD statusCode = 0, statusSize = sizeof(statusCode);
        WinHttpQueryHeaders(hRequest, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
            nullptr, &statusCode, &statusSize, nullptr);

        if (statusCode == 200) {
            found = true;
            std::cout << "  FOUND in AccurateRip database!\n";
        } else if (statusCode == 404) {
            std::cout << "  NOT FOUND in AccurateRip database\n";
        }
    }

    if (hRequest) WinHttpCloseHandle(hRequest);
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);
    return found;
}

bool AccurateRip::VerifyCRCs(const DiscInfo& disc) {
    std::cout << "\n=== AccurateRip CRC Verification ===\n";
    size_t sectorIdx = 0;
    
    for (size_t i = 0; i < disc.tracks.size(); i++) {
        const auto& t = disc.tracks[i];
        if (!t.isAudio) continue;
        
        // AccurateRip uses INDEX 01 (startLBA), not pregap
        DWORD start = t.startLBA;
        DWORD count = t.endLBA - start + 1;
        
        // Skip sectors if we read pregaps into rawSectors
        if (disc.pregapMode == PregapMode::Include && t.pregapLBA < t.startLBA) {
            DWORD pregapSectors = t.startLBA - t.pregapLBA;
            sectorIdx += pregapSectors;  // Skip pregap data
        }
        
        std::vector<std::vector<BYTE>> trackSectors;
        for (DWORD j = 0; j < count && sectorIdx < disc.rawSectors.size(); j++) {
            std::vector<BYTE> audioOnly(AUDIO_SECTOR_SIZE);
            memcpy(audioOnly.data(), disc.rawSectors[sectorIdx++].data(), AUDIO_SECTOR_SIZE);
            trackSectors.push_back(std::move(audioOnly));
        }
        
        uint32_t crc = CalculateCRC(trackSectors, t.trackNumber, 
            static_cast<int>(disc.tracks.size()), t.startLBA);
        std::cout << "Track " << std::setw(2) << t.trackNumber << ": CRC = "
            << std::hex << std::setw(8) << std::setfill('0') << crc 
            << std::dec << std::setfill(' ') << "\n";
    }
    return true;
}