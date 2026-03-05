// ============================================================================
// ConsoleGraph.h - Rich console bar graph renderer
//
// Uses Unicode vertical block elements (▁▂▃▄▅▆▇█) for 8× vertical
// resolution and ANSI RGB color gradient (green → yellow → red) for
// severity visualization.
// ============================================================================
#pragma once

#include "ConsoleColor.h"
#include "ConsoleSymbols.h"
#include <iostream>
#include <iomanip>
#include <string>
#include <vector>
#include <algorithm>
#include <functional>

namespace Console {

struct GraphOptions {
	std::string title;
	std::string subtitle;
	int width = 60;
	int height = 12;        // rows (each has 8 sub-levels = 96 effective levels)
	int refLine = 0;        // reference line value (e.g., 220 for Red Book)
	std::string refLabel;   // e.g., "Red Book 220/sec"
	bool colorize = true;   // green→yellow→red gradient
};

// Color for a given severity (0.0 = green, 0.5 = yellow, 1.0 = red)
inline void SetBarColor(double severity) {
	int r, g, b;
	if (severity < 0.5) {
		// Green (80,220,120) → Yellow (240,200,60)
		double t = severity * 2.0;
		r = static_cast<int>(80 + t * 160);
		g = static_cast<int>(220 - t * 20);
		b = static_cast<int>(120 - t * 60);
	}
	else {
		// Yellow (240,200,60) → Red (240,80,80)
		double t = (severity - 0.5) * 2.0;
		r = 240;
		g = static_cast<int>(200 - t * 120);
		b = static_cast<int>(60 + t * 20);
	}
	SetColorRGB(r, g, b);
}

// Render a bar chart from bucketed integer data
inline void DrawBarGraph(const std::vector<int>& buckets, int maxVal,
	const GraphOptions& opts, DWORD totalSeconds = 0) {
	if (buckets.empty() || maxVal <= 0) return;

	int width = static_cast<int>(buckets.size());
	int height = opts.height;

	// Title
	SetColorRGB(Theme::CyanR, Theme::CyanG, Theme::CyanB);
	std::cout << "\n" << Sym::TopLeft;
	for (int i = 0; i < width + 8; i++) std::cout << Sym::Horizontal;
	std::cout << Sym::TopRight << "\n";
	SetColorRGB(Theme::WhiteR, Theme::WhiteG, Theme::WhiteB);
	std::cout << "\033[1m  " << opts.title << "\033[22m\n";
	if (!opts.subtitle.empty()) {
		SetColorRGB(Theme::DimR, Theme::DimG, Theme::DimB);
		std::cout << "  " << opts.subtitle << "\n";
	}
	Reset();
	std::cout << "\n";

	// Compute reference line row (which row it falls on)
	int refRow = -1;
	if (opts.refLine > 0 && opts.refLine < maxVal) {
		refRow = (opts.refLine * height) / maxVal;
		if (refRow < 1) refRow = 1;
		if (refRow >= height) refRow = -1;
	}

	int labelW = std::max(5, static_cast<int>(std::to_string(maxVal).length()) + 1);

	// Each row represents maxVal/height units. Within each row, the 8 block
	// element levels give sub-row precision.
	for (int row = height; row >= 1; row--) {
		// Y-axis label
		if (row == height) {
			SetColorRGB(Theme::DimR, Theme::DimG, Theme::DimB);
			std::cout << std::setw(labelW) << maxVal;
		}
		else if (row == refRow && opts.refLine > 0) {
			SetColorRGB(Theme::YellowR, Theme::YellowG, Theme::YellowB);
			std::cout << std::setw(labelW) << opts.refLine;
		}
		else if (row == (height + 1) / 2 && row != refRow) {
			SetColorRGB(Theme::DimR, Theme::DimG, Theme::DimB);
			std::cout << std::setw(labelW) << (maxVal / 2);
		}
		else if (row == 1) {
			SetColorRGB(Theme::DimR, Theme::DimG, Theme::DimB);
			std::cout << std::setw(labelW) << 0;
		}
		else {
			std::cout << std::string(labelW, ' ');
		}

		// Axis
		SetColorRGB(Theme::DimR, Theme::DimG, Theme::DimB);
		std::cout << " " << Sym::Vertical;

		// Bar cells
		double rowBase = static_cast<double>(row - 1) / height * maxVal;
		double rowTop = static_cast<double>(row) / height * maxVal;
		double rowRange = rowTop - rowBase;

		for (int col = 0; col < width; col++) {
			double val = static_cast<double>(buckets[col]);

			if (val >= rowTop) {
				// Full block
				double severity = val / maxVal;
				if (opts.colorize) SetBarColor(severity);
				else SetColorRGB(Theme::CyanR, Theme::CyanG, Theme::CyanB);
				std::cout << Sym::Bar8;
			}
			else if (val > rowBase) {
				// Partial block (1-8 levels)
				double fill = (val - rowBase) / rowRange;
				int level = static_cast<int>(fill * 8.0);
				if (level < 1) level = 1;
				if (level > 8) level = 8;
				double severity = val / maxVal;
				if (opts.colorize) SetBarColor(severity);
				else SetColorRGB(Theme::CyanR, Theme::CyanG, Theme::CyanB);
				std::cout << Sym::BarLevels[level];
			}
			else if (row == refRow && opts.refLine > 0) {
				// Reference line through empty cells
				SetColorRGB(Theme::YellowR, Theme::YellowG, Theme::YellowB);
				std::cout << "\xe2\x94\x80";  // ─
			}
			else {
				std::cout << " ";
			}
		}
		Reset();
		std::cout << "\n";
	}

	// X-axis
	SetColorRGB(Theme::DimR, Theme::DimG, Theme::DimB);
	std::cout << std::string(labelW, ' ') << " " << Sym::BottomLeft;
	for (int i = 0; i < width; i++) std::cout << Sym::Horizontal;
	std::cout << Sym::BottomRight << "\n";

	// Time labels
	int padding = labelW + 2;
	if (totalSeconds > 0) {
		int endMin = totalSeconds / 60;
		int endSec = totalSeconds % 60;
		char endStr[16];
		snprintf(endStr, sizeof(endStr), "%d:%02d", endMin, endSec);

		SetColorRGB(Theme::DimR, Theme::DimG, Theme::DimB);
		std::cout << std::string(padding, ' ') << "0:00";
		int gap = width - 4 - static_cast<int>(strlen(endStr));
		if (gap > 0) std::cout << std::string(gap, ' ');
		std::cout << endStr << "\n";
	}

	// Reference line legend
	if (opts.refLine > 0 && !opts.refLabel.empty()) {
		SetColorRGB(Theme::YellowR, Theme::YellowG, Theme::YellowB);
		std::cout << std::string(padding, ' ')
			<< "\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80 " << opts.refLabel << "\n";
	}

	Reset();
}

// Helper: bucket raw per-second data into graph columns, taking peak per bucket
inline std::vector<int> BucketData(const std::vector<int>& perSecond, int width) {
	std::vector<int> buckets(width, 0);
	if (perSecond.empty()) return buckets;
	for (size_t i = 0; i < perSecond.size(); i++) {
		size_t b = (i * width) / perSecond.size();
		if (b >= static_cast<size_t>(width)) b = width - 1;
		buckets[b] = std::max(buckets[b], perSecond[i]);
	}
	return buckets;
}

}  // namespace Console