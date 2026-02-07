#pragma once
#include <windows.h>
#include <iostream>

namespace Console {
    enum class Color : WORD {
        Black = 0,
        DarkBlue = 1,
        DarkGreen = 2,
        DarkCyan = 3,
        DarkRed = 4,
        DarkMagenta = 5,
        DarkYellow = 6,
        Gray = 7,
        DarkGray = 8,
        Blue = 9,
        Green = 10,
        Cyan = 11,
        Red = 12,
        Magenta = 13,
        Yellow = 14,
        White = 15
    };

    inline void SetColor(Color fg, Color bg = Color::Black) {
        SetConsoleTextAttribute(GetStdHandle(STD_OUTPUT_HANDLE),
            static_cast<WORD>(fg) | (static_cast<WORD>(bg) << 4));
    }

    inline void Reset() {
        SetColor(Color::Gray);
    }

    inline void Success(const char* msg) {
        SetColor(Color::Green);
        std::cout << msg;
        Reset();
    }

    inline void Error(const char* msg) {
        SetColor(Color::Red);
        std::cout << msg;
        Reset();
    }

    inline void Warning(const char* msg) {
        SetColor(Color::Yellow);
        std::cout << msg;
        Reset();
    }

    inline void Info(const char* msg) {
        SetColor(Color::Cyan);
        std::cout << msg;
        Reset();
    }

    inline void Heading(const char* msg) {
        SetColor(Color::White);
        std::cout << msg;
        Reset();
    }
}