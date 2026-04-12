// ============================================================================
// ScsiDrive.cpp - Implementation moved to ScsiDrive.*.cpp modules
// ============================================================================

bool ScsiDrive::GetDriveSpecificConfig(DriveSpecificConfig& config) const {
	std::string vendor = m_vendor;
	std::string model = m_model;
	
	// Normalize strings (trim, uppercase)
	auto normalize = [](std::string& s) {
		// Trim leading/trailing whitespace
		size_t start = s.find_first_not_of(" \t\r\n");
		size_t end = s.find_last_not_of(" \t\r\n");
		if (start == std::string::npos) {
			s.clear();
			return;
		}
		s = s.substr(start, end - start + 1);
		
		// Convert to uppercase
		for (char& c : s) c = toupper(c);
	};
	
	normalize(vendor);
	normalize(model);
	
	// Search known configurations
	for (const auto& knownConfig : knownDriveConfigs) {
		std::string knownVendor = knownConfig.vendor;
		std::string knownModel = knownConfig.model;
		normalize(knownVendor);
		normalize(knownModel);
		
		if (vendor == knownVendor && model.find(knownModel) != std::string::npos) {
			config = knownConfig;
			return true;
		}
	}
	
	return false;
}

bool ScsiDrive::ApplyOptimalAudioSettings() {
	DriveSpecificConfig config;
	if (!GetDriveSpecificConfig(config)) {
		// No specific config, use defaults
		return false;
	}
	
	// Apply read speed recommendation
	if (config.recommendedReadSpeed > 0) {
		SetReadSpeed(config.recommendedReadSpeed);
	}
	
	// Store offset for audio extraction
	m_readOffset = config.readOffset;
	
	// Enable/disable C2 error reporting as recommended
	if (config.forceC2ErrorReporting) {
		// Enable C2 if supported
		DriveCapabilities caps;
		if (DetectCapabilities(caps) && caps.supportsC2ErrorReporting) {
			m_useC2ErrorDetection = true;
		}
	} else if (config.disableC2ErrorReporting) {
		m_useC2ErrorDetection = false;
	}
	
	return true;
}