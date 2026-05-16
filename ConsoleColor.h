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
	namespace Theme {

		// background
		constexpr int BgR = 10, BgG = 10, BgB = 11;

		// standard text
		constexpr int FgR = 188, FgG = 184, FgB = 176;

		// bright metallic text
		constexpr int WhiteR = 228, WhiteG = 224, WhiteB = 218;

		// inactive / dim
		constexpr int DimR = 92, DimG = 88, DimB = 84;

		// menu / active UI highlight
		constexpr int MenuR = 242;
		constexpr int MenuG = 156;
		constexpr int MenuB = 52;

		// secondary amber
		constexpr int YellowR = 218;
		constexpr int YellowG = 118;
		constexpr int YellowB = 32;

		// steel-blue accent
		constexpr int CyanR = 76, CyanG = 118, CyanB = 142;

		// muted green
		constexpr int GreenR = 134, GreenG = 148, GreenB = 116;

		// copper-red
		constexpr int RedR = 152, RedG = 82, RedB = 60;

	}

	inline void SetColor(Color fg, Color bg = Color::Black) {
		SetConsoleTextAttribute(GetStdHandle(STD_OUTPUT_HANDLE),
			static_cast<WORD>(fg) | (static_cast<WORD>(bg) << 4));
	}

	inline void Reset() {
		std::cout << "\033[0m";
		// Only set foreground — let Windows Terminal handle the background
		SetColorRGB(Theme::FgR, Theme::FgG, Theme::FgB);
	}

	// ── Theme application ────────────────────────────────────
	// Uses ANSI escapes to set background + clear screen.
	// Works in both Windows Terminal and conhost.
	inline void ApplyDarkTheme() {
		// Set only foreground color — background comes from Windows Terminal
		SetColorRGB(Theme::FgR, Theme::FgG, Theme::FgB);
		std::cout << "\033[2J"   // clear screen
		          << "\033[H";   // cursor to top-left
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