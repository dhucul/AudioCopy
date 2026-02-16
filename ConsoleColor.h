// ============================================================================
// ConsoleColor.h - Core color functions and theme
// ============================================================================
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

	// ── ANSI RGB color helpers (work in Windows Terminal) ─────
	inline void SetColorRGB(int fgR, int fgG, int fgB) {
		std::cout << "\033[38;2;" << fgR << ";" << fgG << ";" << fgB << "m";
	}

	inline void SetBgRGB(int bgR, int bgG, int bgB) {
		std::cout << "\033[48;2;" << bgR << ";" << bgG << ";" << bgB << "m";
	}

	// Default theme colors (RGB)
	namespace Theme {
		constexpr int BgR = 30,  BgG = 30,  BgB = 40;   // background
		constexpr int FgR = 200, FgG = 200, FgB = 210;   // default text
		constexpr int CyanR = 90,  CyanG = 200, CyanB = 240;
		constexpr int GreenR = 80,  GreenG = 220, GreenB = 120;
		constexpr int RedR = 240, RedG = 80,  RedB = 80;
		constexpr int YellowR = 240, YellowG = 200, YellowB = 60;
		constexpr int WhiteR = 255, WhiteG = 255, WhiteB = 255;
		constexpr int DimR = 100, DimG = 100, DimB = 120;
	}

	inline void SetColor(Color fg, Color bg = Color::Black) {
		SetConsoleTextAttribute(GetStdHandle(STD_OUTPUT_HANDLE),
			static_cast<WORD>(fg) | (static_cast<WORD>(bg) << 4));
	}

	inline void Reset() {
		// Reset ANSI formatting, then set theme background + foreground
		std::cout << "\033[0m";
		SetBgRGB(Theme::BgR, Theme::BgG, Theme::BgB);
		SetColorRGB(Theme::FgR, Theme::FgG, Theme::FgB);
	}

	// ── Theme application ────────────────────────────────────
	// Uses ANSI escapes to set background + clear screen.
	// Works in both Windows Terminal and conhost.
	inline void ApplyDarkTheme() {
		// Set background color and clear entire screen with it
		SetBgRGB(Theme::BgR, Theme::BgG, Theme::BgB);
		SetColorRGB(Theme::FgR, Theme::FgG, Theme::FgB);
		std::cout << "\033[2J"   // clear screen
		          << "\033[H";   // cursor to top-left
		// Set console title
		SetConsoleTitleW(L"Audio CD Copy Tool");
	}

	// ── Font setup (works in conhost, no-op in Windows Terminal) ──
	inline void SetFont(const wchar_t* fontName = L"Cascadia Mono", short fontSize = 18) {
		CONSOLE_FONT_INFOEX cfi = {};
		cfi.cbSize = sizeof(cfi);
		cfi.dwFontSize.Y = fontSize;
		cfi.FontFamily = FF_DONTCARE;
		cfi.FontWeight = FW_NORMAL;
		wcscpy_s(cfi.FaceName, fontName);
		SetCurrentConsoleFontEx(GetStdHandle(STD_OUTPUT_HANDLE), FALSE, &cfi);
	}

	inline void SetFontBold(const wchar_t* fontName = L"Cascadia Mono", short fontSize = 18) {
		CONSOLE_FONT_INFOEX cfi = {};
		cfi.cbSize = sizeof(cfi);
		cfi.dwFontSize.Y = fontSize;
		cfi.FontFamily = FF_DONTCARE;
		cfi.FontWeight = FW_BOLD;
		wcscpy_s(cfi.FaceName, fontName);
		SetCurrentConsoleFontEx(GetStdHandle(STD_OUTPUT_HANDLE), FALSE, &cfi);
	}

	inline void SetWindowSize(short cols = 100, short rows = 40) {
		HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
		SMALL_RECT tiny = { 0, 0, 1, 1 };
		SetConsoleWindowInfo(hOut, TRUE, &tiny);
		COORD bufferSize = { cols, 300 };
		SetConsoleScreenBufferSize(hOut, bufferSize);
		SMALL_RECT window = { 0, 0, static_cast<SHORT>(cols - 1), static_cast<SHORT>(rows - 1) };
		SetConsoleWindowInfo(hOut, TRUE, &window);
	}
}