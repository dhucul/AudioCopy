// ============================================================================
// ConsoleFormat.h - Formatted text output helpers
// ============================================================================
#pragma once

#include "ConsoleColor.h"
#include <iostream>

namespace Console {
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
}