// ============================================================================
// MenuHelpers.h - Console menu input utilities
// ============================================================================
#pragma once

#include "DiscTypes.h"
#include "InterruptHandler.h"
#include <iomanip>
#include <iostream>
#include <string>

inline int GetMenuChoice(int minChoice, int maxChoice, int defaultChoice = 1) {
    if (g_interrupt.IsInterrupted() || g_interrupt.CheckEscapeKey()) {
        g_interrupt.SetInterrupted(true);
        std::cout << "\n\n*** Cancelled by user ***\n";
        std::cout << "Press any key to exit...\n";
        _getch();
        exit(0);
    }

    int choice;
    if (!(std::cin >> choice) || choice < minChoice || choice > maxChoice) {
        std::cin.clear();
        std::cin.ignore(10000, '\n');

        if (g_interrupt.IsInterrupted() || g_interrupt.CheckEscapeKey()) {
            g_interrupt.SetInterrupted(true);
            std::cout << "\n\n*** Cancelled by user ***\n";
            std::cout << "Press any key to exit...\n";
            _getch();
            exit(0);
        }

        std::cout << "Invalid choice, using default (" << defaultChoice << ")\n";
        return defaultChoice;
    }
    return choice;
}

inline void PrintDiscInfo(const DiscInfo& disc) {
    std::cout << "\n=== Disc Info ===\n";
    if (!disc.cdText.albumTitle.empty())
        std::cout << "Album: " << disc.cdText.albumTitle << "\n";
    if (!disc.cdText.albumArtist.empty())
        std::cout << "Artist: " << disc.cdText.albumArtist << "\n";
    std::cout << "Sessions: " << disc.sessionCount << "\n";
    std::cout << "Tracks: " << disc.tracks.size();
    if (disc.tocRepaired)
        std::cout << "  (reconstructed from Q subchannel)";
    std::cout << "\n";
    std::cout << "Lead-out: LBA " << disc.leadOutLBA << "\n";

    // Count audio vs data for summary
    int audioCount = 0, dataCount = 0;
    for (const auto& t : disc.tracks) {
        if (t.isAudio) audioCount++;
        else dataCount++;
    }
    if (dataCount > 0)
        std::cout << "  (" << audioCount << " audio, " << dataCount << " data)\n";

    std::cout << "\n";

    for (size_t i = 0; i < disc.tracks.size(); i++) {
        const auto& t = disc.tracks[i];
        DWORD sectors = (t.endLBA >= t.startLBA) ? (t.endLBA - t.startLBA + 1) : 0;
        int sec = static_cast<int>(sectors / 75);

        // Pregap calculation
        int pregapFrames = 0;
        if (t.startLBA == 0) {
            // Track 1: pregap is either index01LBA (from TOC subchannel scan)
            // or startLBA - pregapLBA if ProbePregap found INDEX 00
            if (t.index01LBA > 0)
                pregapFrames = static_cast<int>(t.index01LBA);
            // ProbePregap can't set pregapLBA < 0 for track 1 (startLBA=0),
            // so no pregap to show here.
        }
        else if (t.pregapLBA < t.startLBA) {
            pregapFrames = static_cast<int>(t.startLBA - t.pregapLBA);
        }

        // Detect gap between this track and the previous one
        bool hasGapBefore = false;
        if (i > 0) {
            DWORD prevEnd = disc.tracks[i - 1].endLBA;
            hasGapBefore = (t.startLBA > prevEnd + 225);
        }

        // Track line
        std::cout << "  Track " << std::setw(2) << t.trackNumber
            << " [S" << t.session << "]: "
            << (t.isAudio ? "Audio" : "Data ") << " "
            << sec / 60 << ":"
            << std::setfill('0') << std::setw(2) << sec % 60
            << std::setfill(' ')
            << " (" << std::setw(3) << sec << "s)"
            << "  LBA " << std::setw(6) << t.startLBA
            << " - " << std::setw(6) << t.endLBA;

        // Pregap
        if (pregapFrames > 0) {
            int pgSec = pregapFrames / 75;
            int pgFrm = pregapFrames % 75;
            std::cout << "  Pregap ";
            if (pgSec > 0)
                std::cout << pgSec << "s ";
            std::cout << pgFrm << "f";
        }

        // Flags
        if (t.hasPreemphasis) std::cout << "  [Pre-emphasis]";
        if (!t.isrc.empty())  std::cout << "  ISRC:" << t.isrc;
        if (hasGapBefore)     std::cout << "  [GAP]";

        std::cout << "\n";
    }
}