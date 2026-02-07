// ============================================================================
// MenuHelpers.h - Console menu input utilities
// ============================================================================
#pragma once

#include "InterruptHandler.h"
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
    std::cout << "Tracks: " << disc.tracks.size() << "\n";

    for (const auto& t : disc.tracks) {
        int sec = (t.endLBA - t.startLBA + 1) / 75;
        std::cout << "  Track " << t.trackNumber << " [S" << t.session << "]: "
            << (t.isAudio ? "Audio" : "Data") << " "
            << sec / 60 << ":" << std::setfill('0') << std::setw(2) << sec % 60
            << std::setfill(' ') << " (" << sec << " seconds)\n";
    }
}