// ============================================================================
// AccurateRip.h - AccurateRip CRC calculation and database lookup
// ============================================================================
#pragma once

#include "DiscTypes.h"
#include <cstdint>

class AccurateRip {
public:
    // CRC calculation
    static uint32_t CalculateCRC(const std::vector<std::vector<BYTE>>& sectors,
        int trackNum, int totalTracks, DWORD trackStart);

    // Disc ID calculations
    static uint32_t CalculateDiscID1(const DiscInfo& disc);
    static uint32_t CalculateDiscID2(const DiscInfo& disc);
    static uint32_t CalculateCDDBID(const DiscInfo& disc);

    // Database lookup
    static bool Lookup(DiscInfo& disc);

    // Verify CRCs for all tracks
    static bool VerifyCRCs(const DiscInfo& disc);
};