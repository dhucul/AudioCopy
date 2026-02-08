// ============================================================================
// AudioCDCopier.cpp - Main audio CD copying implementation
// ============================================================================
#define NOMINMAX

#include "AudioCDCopier.h"
#include "AccurateRip.h"
#include "InterruptHandler.h"
#include "MenuHelpers.h"
#include <ntddcdrm.h>
#include <iostream>
#include <fstream>
#include <iomanip>
#include <chrono>
#include <algorithm>
#include <map>
#include <cmath>
#include <sstream>
#include <ctime>
#include <conio.h>

// Sentinel value for "back to menu" in all Select* functions
constexpr int MENU_BACK = -1;
