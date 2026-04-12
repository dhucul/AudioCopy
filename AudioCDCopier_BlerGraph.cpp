#define NOMINMAX
#include "AudioCDCopier.h"
#include <iostream>
#include <iomanip>
#include <algorithm>

// ============================================================================
// BLER Graphing - Visual Error Distribution Charts
// ============================================================================

void AudioCDCopier::PrintBlerGraph(const BlerResult& result, int width, int height) {
	if (result.perSecondC2.empty() || width <= 0 || height <= 0) return;

	// C1 Error Distribution Graph
	if (result.hasC1Data && !result.perSecondC1.empty()) {
		std::cout << "\n=== C1 Error Distribution ===\n";
		std::cout << "  Each column = a time slice of the disc; height = C1 error count\n\n";

		int maxC1 = 1;
		for (const auto& p : result.perSecondC1) {
			if (p.second > maxC1) maxC1 = p.second;
		}

		std::vector<int> c1Buckets(width, 0);
		size_t c1DataSize = result.perSecondC1.size();
		for (size_t i = 0; i < c1DataSize; i++) {
			size_t bucket = (i * static_cast<size_t>(width)) / c1DataSize;
			if (bucket >= static_cast<size_t>(width)) bucket = static_cast<size_t>(width) - 1;
			c1Buckets[bucket] = std::max(c1Buckets[bucket], result.perSecondC1[i].second);
		}

		int labelWidth = std::max(4, static_cast<int>(std::to_string(maxC1).length()) + 1);

		for (int row = height; row > 0; row--) {
			int threshold = std::max(1, (maxC1 * row) / height);

			if (row == height)
				std::cout << std::setw(labelWidth) << maxC1 << " |";
			else if (row == (height + 1) / 2)
				std::cout << std::setw(labelWidth) << (maxC1 / 2) << " |";
			else if (row == 1)
				std::cout << std::setw(labelWidth) << 0 << " |";
			else
				std::cout << std::string(labelWidth, ' ') << " |";

			for (int col = 0; col < width; col++) {
				if (c1Buckets[col] >= threshold) {
					double severity = static_cast<double>(c1Buckets[col]) / maxC1;
					if (severity > 0.66) std::cout << '#';
					else if (severity > 0.33) std::cout << '+';
					else std::cout << '.';
				}
				else {
					std::cout << ' ';
				}
			}
			std::cout << "\n";
		}

		std::cout << std::string(labelWidth, ' ') << " +" << std::string(width, '-') << "\n";

		int padding = labelWidth + 2;
		int endMin = result.totalSeconds / 60;
		int endSec = result.totalSeconds % 60;
		std::string endStr = std::to_string(endMin) + ":"
			+ (endSec < 10 ? "0" : "") + std::to_string(endSec);

		std::cout << std::string(padding, ' ') << "0:00";
		int gap = width - 4 - static_cast<int>(endStr.length());
		if (gap > 0) std::cout << std::string(gap, ' ');
		std::cout << endStr << "\n";

		std::cout << std::string(padding, ' ')
			<< "# = high (>66%)  + = moderate (33-66%)  . = low (<33%)\n";

		if (maxC1 > 220) {
			std::cout << std::string(padding, ' ')
				<< "!! Peak C1/sec (" << maxC1 << ") exceeds Red Book BLER limit (220/sec)\n";
		}
	}

	// C2 Error Distribution Graph
	if (result.totalC2Errors == 0 && result.totalReadFailures == 0) {
		std::cout << "\n=== C2 Error Distribution ===\n";
		std::cout << "  No C2 errors — graph skipped.\n";
	}
	else {
		std::cout << "\n=== C2 Error Distribution ===\n";
		std::cout << "  Each column = a time slice of the disc; height = C2 error count\n\n";

		int maxC2 = 1;
		for (const auto& p : result.perSecondC2) {
			if (p.second > maxC2) maxC2 = p.second;
		}

		std::vector<int> buckets(width, 0);
		size_t dataSize = result.perSecondC2.size();
		for (size_t i = 0; i < dataSize; i++) {
			size_t bucket = (i * static_cast<size_t>(width)) / dataSize;
			if (bucket >= static_cast<size_t>(width)) bucket = static_cast<size_t>(width) - 1;
			buckets[bucket] = std::max(buckets[bucket], result.perSecondC2[i].second);
		}

		int labelWidth = std::max(4, static_cast<int>(std::to_string(maxC2).length()) + 1);

		for (int row = height; row > 0; row--) {
			int threshold = std::max(1, (maxC2 * row) / height);

			if (row == height)
				std::cout << std::setw(labelWidth) << maxC2 << " |";
			else if (row == (height + 1) / 2)
				std::cout << std::setw(labelWidth) << (maxC2 / 2) << " |";
			else if (row == 1)
				std::cout << std::setw(labelWidth) << 0 << " |";
			else
				std::cout << std::string(labelWidth, ' ') << " |";

			for (int col = 0; col < width; col++) {
				if (buckets[col] >= threshold) {
					double severity = static_cast<double>(buckets[col]) / maxC2;
					if (severity > 0.66) std::cout << '#';
					else if (severity > 0.33) std::cout << '+';
					else std::cout << '.';
				}
				else {
					std::cout << ' ';
				}
			}
			std::cout << "\n";
		}

		std::cout << std::string(labelWidth, ' ') << " +" << std::string(width, '-') << "\n";

		int padding = labelWidth + 2;
		int endMin = result.totalSeconds / 60;
		int endSec = result.totalSeconds % 60;
		std::string endStr = std::to_string(endMin) + ":"
			+ (endSec < 10 ? "0" : "") + std::to_string(endSec);

		std::cout << std::string(padding, ' ') << "0:00";
		int gap = width - 4 - static_cast<int>(endStr.length());
		if (gap > 0) std::cout << std::string(gap, ' ');
		std::cout << endStr << "\n";

		std::cout << std::string(padding, ' ')
			<< "# = high (>66%)  + = moderate (33-66%)  . = low (<33%)\n";
	}
}