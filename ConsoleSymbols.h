// ============================================================================
// ConsoleSymbols.h - Unicode symbols and special characters
// ============================================================================
#pragma once

namespace Console {
	namespace Sym {
		// Status symbols
		constexpr const char* Check = "\xe2\x9c\x93";
		constexpr const char* Cross = "\xe2\x9c\x97";
		constexpr const char* Warn = "\xe2\x9a\xa0";
		constexpr const char* InfoSym = "\xe2\x84\xb9";
		constexpr const char* Bullet = "\xe2\x80\xa2";
		constexpr const char* Arrow = "\xe2\x96\xb6";
		constexpr const char* Disc = "\xf0\x9f\x92\xbf";
		constexpr const char* Music = "\xe2\x99\xab";

		// Box drawing
		constexpr const char* TopLeft = "\xe2\x94\x8c";
		constexpr const char* TopRight = "\xe2\x94\x90";
		constexpr const char* BottomLeft = "\xe2\x94\x94";
		constexpr const char* BottomRight = "\xe2\x94\x98";
		constexpr const char* Horizontal = "\xe2\x94\x80";
		constexpr const char* Vertical = "\xe2\x94\x82";
		constexpr const char* TeeRight = "\xe2\x94\x9c";
		constexpr const char* TeeLeft = "\xe2\x94\xa4";

		// Block characters
		constexpr const char* BlockFull = "\xe2\x96\x88";
		constexpr const char* BlockLight = "\xe2\x96\x91";
	}
}