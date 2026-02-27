// ============================================================================
// DriveOffsets.h - AccurateRip drive offset database
// Source: http://www.accuraterip.com/driveoffsets.htm
// ============================================================================
#pragma once

struct DriveOffsetEntry {
    const char* vendor;
    const char* model;
    int offset;
};

// AccurateRip verified drive offsets
static const DriveOffsetEntry knownOffsets[] = {
    // Pioneer drives
    {"PIONEER", "BDR-S13U", +667},   // Verified: 91 submissions, 100% agreement
    {"PIONEER", "BD-RW BDR-S12U", +667},
    {"PIONEER", "BD-RW BDR-S12", +667},
    {"PIONEER", "BD-RW BDR-S11", +667},
    {"PIONEER", "BD-RW BDR-S09", +667},
    {"PIONEER", "BD-RW BDR-S08", +667},
    {"PIONEER", "BD-RW BDR-209", +667},
    {"PIONEER", "BD-RW BDR-208", +667},
    {"PIONEER", "BD-RW BDR-207", +667},
    {"PIONEER", "BD-RW BDR-206", +667},
    {"PIONEER", "BD-RW BDR-205", +667},
    {"PIONEER", "BD-RW BDR-XD07", +667},
    {"PIONEER", "BD-RW BDR-XD05", +667},
    {"PIONEER", "BD-RW BDR-XD04", +667},
    {"PIONEER", "BD-ROM BDC-207", +667},
    {"PIONEER", "BD-ROM BDC-202", +667},
    {"PIONEER", "DVD-RW DVR-221", +6},
    {"PIONEER", "DVD-RW DVR-220", +6},
    {"PIONEER", "DVD-RW DVR-219", +6},
    {"PIONEER", "DVD-RW DVR-218", +6},
    {"PIONEER", "DVD-RW DVR-217", +96},
    {"PIONEER", "DVD-RW DVR-216", +96},
    {"PIONEER", "DVD-RW DVR-215", +48},
    {"PIONEER", "DVD-RW DVR-212", +48},
    {"PIONEER", "DVD-RW DVR-115", +48},
    {"PIONEER", "DVD-RW DVR-112", +48},
    {"PIONEER", "DVD-RW DVR-111", +48},
    {"PIONEER", "DVD-RW DVR-110", +48},
    {"PIONEER", "DVD-RW DVR-109", +48},
    {"PIONEER", "DVD-RW DVR-108", +48},
    {"PIONEER", "DVD-RW DVR-107", +48},
    {"PIONEER", "DVD-RW DVR-106", +48},
    {"PIONEER", "DVD-RW DVR-105", +48},
    {"PIONEER", "DVD-RW DVR-104", +48},
    {"PIONEER", "DVD-ROM DVD-106", +102},
    {"PIONEER", "DVD-ROM DVD-116", +102},
    {"PIONEER", "DVD-ROM DVD-117", +102},
    
    // Plextor drives
    {"PLEXTOR", "CD-R PREMIUM2", +30},
    {"PLEXTOR", "CD-R PREMIUM", +30},
    {"PLEXTOR", "DVDR PX-760A", +30},
    {"PLEXTOR", "DVDR PX-755A", +30},
    {"PLEXTOR", "DVDR PX-716A", +30},
    {"PLEXTOR", "DVDR PX-712A", +30},
    {"PLEXTOR", "DVDR PX-708A", +30},
    {"PLEXTOR", "DVDR PX-891SAF", +6},
    {"PLEXTOR", "PX-891SAF", +6},
    {"PLEXTOR", "PX-891SAF PLUS", +6},
    {"PLEXTOR", "DVDR PX-891SA", +6},
    {"PLEXTOR", "DVDR PX-880SA", +6},
    {"PLEXTOR", "DVDR PX-L890SA", +6},
    {"PLEXTOR", "DVDR PX-810SA", +48},
    {"PLEXTOR", "DVDR PX-800A", +48},
    {"PLEXTOR", "DVDR PX-740A", +618},
    {"PLEXTOR", "DVDR PX-750A", +102},
    {"PLEXTOR", "BD-R PX-B950SA", +6},
    {"PLEXTOR", "BD-R PX-LB950SA", +6},
    {"PLEXTOR", "BD-R PX-B940SA", +667},
    {"PLEXTOR", "BD-R PX-B920SA", +667},
    {"PLEXTOR", "CD-R PX-W5224A", +30},
    {"PLEXTOR", "CD-R PX-W4824A", +98},
    {"PLEXTOR", "CD-R PX-W4012A", +98},
    {"PLEXTOR", "CD-R PX-W2410A", +98},
    {"PLEXTOR", "CD-R PX-W1210A", +99},
    {"PLEXTOR", "CD-ROM PX-40TS", +676},
    {"PLEXTOR", "DVD-ROM PX-130A", +738},
    
    // LG Electronics drives
    {"LG", "BD-RE WH16NS60", +6},
    {"LG", "BD-RE WH16NS40", +6},
    {"LG", "BD-RE WH14NS40", +6},
    {"LG", "BD-RE BH16NS55", +6},
    {"LG", "BD-RE BH16NS40", +6},
    {"LG", "BD-RE BH14NS40", +6},
    {"LG", "BD-RE BH10LS30", +667},
    {"LG", "BD-RE GGW-H20L", +667},
    {"LG", "DVDRAM GH24NSD1", +6},
    {"LG", "DVDRAM GH24NSB0", +6},
    {"LG", "DVDRAM GH24NS95", +6},
    {"LG", "DVDRAM GH24NS90", +6},
    {"LG", "DVDRAM GH22NS50", +667},
    {"LG", "DVDRAM GH22NS40", +667},
    {"LG", "DVDRAM GSA-4167B", +667},
    {"LG", "DVDRAM GSA-4163B", +667},
    {"LG", "DVD-ROM GDR8164B", +102},
    {"LG", "DVD-ROM GDR8163B", +102},
    {"LG Electronics", "BD-RE WH16NS60", +6},
    {"LG Electronics", "BD-RE BH16NS40", +6},
    {"LG Electronics", "DVDRAM GH24NSD5", +6},
    {"LG Electronics", "DVDRAM GP65NB60", +6},
    {"LG Electronics", "DVDRAM GP60NB50", +6},
    {"LG Electronics", "DVDRAM GP57EB40", +6},
    
    // ASUS drives  
    {"ASUS", "BW-16D1HT", +6},
    {"ASUS", "BW-16D1H-U", +6},     // External USB version - same offset as internal
    {"ASUS", "BW-12B1ST", +6},
    {"ASUS", "BC-12D2HT", +6},
    {"ASUS", "BC-12B1ST", +702},
    {"ASUS", "DRW-24D5MT", +6},
    {"ASUS", "DRW-24F1ST", +6},
    {"ASUS", "DRW-24B1ST", +6},
    {"ASUS", "SDRW-08U9M-U", +6},
    {"ASUS", "SDRW-08U7M-U", +6},
    {"ASUS", "SBW-06D5H-U", +667},
    
    // LITE-ON drives
    {"LITE-ON", "LTR-52327S", +6},
    {"LITE-ON", "LTR-52246S", +6},
    {"LITE-ON", "DVDRW LH-20A1H", +6},
    {"LITE-ON", "DVDRW SHW-1635S", +6},
    {"LITE-ON", "iHAS124", +6},
    {"LITE-ON", "iHBS112", +6},
    {"LITEON", "CD-ROM LTN486S", +600},
    
    // Sony drives
    {"SONY", "BD RW BWU-500S", +6},
    {"SONY", "DVD RW DRU-870S", +48},
    {"SONY", "DVD RW DRU-860S", +6},
    {"SONY", "DVD RW DRU-840A", +6},
    {"SONY", "DVD RW AW-G170A", +48},
    {"SONY", "CD-RW CRX230E", +6},
    
    // Samsung/TSSTcorp drives
    {"TSSTcorp", "CDDVDW SH-224DB", +6},
    {"TSSTcorp", "CDDVDW SH-224BB", +6},
    {"TSSTcorp", "CDDVDW SH-222BB", +6},
    {"TSSTcorp", "CDDVDW SH-S203B", +6},
    {"TSSTcorp", "BDDVDW SE-506CB", +6},
    {"SAMSUNG", "DVD-ROM SD-616E", +12},
    
    // NEC/Optiarc drives
    {"NEC", "DVD_RW ND-3550A", +48},
    {"NEC", "DVD_RW ND-3540A", +48},
    {"NEC", "DVD_RW ND-3520A", +48},
    {"NEC", "DVD_RW ND-3500AG", +48},
    {"NEC", "DVD_RW ND-2500A", +48},
    {"Optiarc", "DVD RW AD-7280S", +48},
    {"Optiarc", "DVD RW AD-7260S", +48},
    {"Optiarc", "DVD RW AD-7240S", +48},
    {"Optiarc", "DVD RW AD-7200S", +48},
    {"Optiarc", "BD RW BD-5750H", +48},
    
    // Panasonic/Matshita drives
    {"Panasonic", "BD-MLT UJ272", +103},
    {"Panasonic", "BD-MLT UJ260", +103},
    {"Panasonic", "DVD-RAM UJ8E2", +103},
    {"Panasonic", "DVD-RAM UJ8C2", +103},
    {"Panasonic", "DVD-RAM SW-9590", +102},
    
    // TEAC drives
    {"TEAC", "DV-W28S-V", +96},
    {"TEAC", "DV-W5600S", +48},
    {"TEAC", "CD-W540E", +686},
    
    // HP drives
    {"hp", "DVDRAM GH40L", +667},
    {"hp", "BD-RE BH30L", +667},
    {"hp", "CDDVDW TS-H653R", +6},
    
    // BENQ drives
    {"BENQ", "DVD DD DW1640", +618},
    {"BENQ", "DVD DD DW1620", +618},
    
    // Yamaha drives
    {"YAMAHA", "CRW-F1E", +733},
    {"YAMAHA", "CRW3200E", +733},
    
    // Dell drives
    {"Dell", "DVD+--RW DW316", +6},
    
    // Lenovo drives
    {"Lenovo", "Slim_USB_Burner", +6},
    
    // HITACHI drives
    {"HITACHI", "DVD-ROM GD-8000", +667},
    {"HITACHI", "DVD-ROM GD-7500", +667},
    
    // TOSHIBA drives
    {"TOSHIBA", "DVD-ROM SD-M1712", -472},
    {"TOSHIBA", "DVD-ROM SD-M1612", -472},
    
    // Generic/ATAPI drives
    {"ATAPI", "iHAS124", +6},
    {"ATAPI", "iHOS104", +696},
    {"HL-DT-ST", "DVDRAM GH24NSB0", +6},
    
    // End marker
    {nullptr, nullptr, 0}
};