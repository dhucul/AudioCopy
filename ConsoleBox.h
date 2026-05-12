// ============================================================================
// ConsoleBox.h - Box-drawing and status display helpers
// ============================================================================
#pragma once

#include "ConsoleColor.h"
#include "ConsoleSymbols.h"
#include <iostream>
#include <cstring>

namespace Console {
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

	// ── Status display helpers ───────────────────────────────
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
}