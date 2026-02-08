#pragma once
#include <windows.h>
#include <iostream>
#include <string>
#include <cstring>

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

	inline void Success(const char* msg) {
		SetColorRGB(Theme::GreenR, Theme::GreenG, Theme::GreenB);
		std::cout << msg;
		Reset();
	}

	inline void Error(const char* msg) {
		SetColorRGB(Theme::RedR, Theme::RedG, Theme::RedB);
		std::cout << msg;
		Reset();
	}

	inline void Warning(const char* msg) {
		SetColorRGB(Theme::YellowR, Theme::YellowG, Theme::YellowB);
		std::cout << msg;
		Reset();
	}

	inline void Info(const char* msg) {
		SetColorRGB(Theme::CyanR, Theme::CyanG, Theme::CyanB);
		std::cout << msg;
		Reset();
	}

	inline void Heading(const char* msg) {
		SetColorRGB(Theme::WhiteR, Theme::WhiteG, Theme::WhiteB);
		std::cout << "\033[1m" << msg << "\033[22m"; // bold on/off
		Reset();
	}

	// ── Unicode symbols & box-drawing ────────────────────────
	namespace Sym {
		constexpr const char* Check = "\xe2\x9c\x93";
		constexpr const char* Cross = "\xe2\x9c\x97";
		constexpr const char* Warn = "\xe2\x9a\xa0";
		constexpr const char* InfoSym = "\xe2\x84\xb9";
		constexpr const char* Bullet = "\xe2\x80\xa2";
		constexpr const char* Arrow = "\xe2\x96\xb6";
		constexpr const char* Disc = "\xf0\x9f\x92\xbf";
		constexpr const char* Music = "\xe2\x99\xab";

		constexpr const char* TopLeft = "\xe2\x94\x8c";
		constexpr const char* TopRight = "\xe2\x94\x90";
		constexpr const char* BottomLeft = "\xe2\x94\x94";
		constexpr const char* BottomRight = "\xe2\x94\x98";
		constexpr const char* Horizontal = "\xe2\x94\x80";
		constexpr const char* Vertical = "\xe2\x94\x82";
		constexpr const char* TeeRight = "\xe2\x94\x9c";
		constexpr const char* TeeLeft = "\xe2\x94\xa4";

		constexpr const char* BlockFull = "\xe2\x96\x88";
		constexpr const char* BlockLight = "\xe2\x96\x91";
	}

	// ── Box-drawing helpers ──────────────────────────────────
	inline void BoxHeading(const char* title, int width = 50) {
		SetColorRGB(Theme::CyanR, Theme::CyanG, Theme::CyanB);
		std::cout << Sym::TopLeft;
		for (int i = 0; i < 2; i++) std::cout << Sym::Horizontal;
		std::cout << " ";
		SetColorRGB(Theme::WhiteR, Theme::WhiteG, Theme::WhiteB);
		std::cout << "\033[1m" << title << "\033[22m";
		SetColorRGB(Theme::CyanR, Theme::CyanG, Theme::CyanB);
		std::cout << " ";
		int titleLen = static_cast<int>(strlen(title));
		int remaining = width - titleLen - 5;
		for (int i = 0; i < remaining; i++) std::cout << Sym::Horizontal;
		std::cout << Sym::TopRight << "\n";
		Reset();
	}

	inline void BoxFooter(int width = 50) {
		SetColorRGB(Theme::CyanR, Theme::CyanG, Theme::CyanB);
		std::cout << Sym::BottomLeft;
		for (int i = 0; i < width - 2; i++) std::cout << Sym::Horizontal;
		std::cout << Sym::BottomRight << "\n";
		Reset();
	}

	inline void BoxSeparator(int width = 50) {
		SetColorRGB(Theme::DimR, Theme::DimG, Theme::DimB);
		std::cout << Sym::TeeRight;
		for (int i = 0; i < width - 2; i++) std::cout << Sym::Horizontal;
		std::cout << Sym::TeeLeft << "\n";
		Reset();
	}

	inline void StatusOk(const char* msg) {
		SetColorRGB(Theme::GreenR, Theme::GreenG, Theme::GreenB);
		std::cout << " " << Sym::Check << " ";
		Reset();
		std::cout << msg << "\n";
	}

	inline void StatusFail(const char* msg) {
		SetColorRGB(Theme::RedR, Theme::RedG, Theme::RedB);
		std::cout << " " << Sym::Cross << " ";
		Reset();
		std::cout << msg << "\n";
	}

	inline void StatusWarn(const char* msg) {
		SetColorRGB(Theme::YellowR, Theme::YellowG, Theme::YellowB);
		std::cout << " " << Sym::Warn << "  ";
		Reset();
		std::cout << msg << "\n";
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