# Pioneer BD Drive Utility Reverse-Engineering Notes

Source workspace: `C:\tmp`

These notes summarize what the decompiled Pioneer BD Drive Utility reveals about Pioneer optical drives, the commands it sends, the capability bytes it reads, and the drive features it exposes.

## High-Level Summary

This utility is a Windows application named **Pioneer BD Drive Utility**. It is decompiled C++/CLI/C# output rather than original clean source.

The program enumerates Windows drive letters, opens optical drives through paths like `\\.\D:`, sends SCSI pass-through commands with `DeviceIoControl`, filters for Pioneer drives, then exposes Pioneer-specific features mostly through vendor usage of `READ BUFFER` and `WRITE BUFFER`.

The code does **not** contain a clean table of supported model numbers such as `BDR-209`, `BDR-XD07`, etc. Drive support is detected dynamically from inquiry strings and a Pioneer capability/status block returned by the drive.

The main files examined were:

- `C:\tmp\CPioneerBDDriveUtility.cs`
- `C:\tmp\CDriveAccess.cs`
- `C:\tmp\CDriveIfSptd.cs`
- `C:\tmp\CDriveInterface.cs`
- `C:\tmp\PioneerBDDriveUtility\CMainForm.cs`
- `C:\tmp\PioneerBDDriveUtility.CMainForm.resx`
- `C:\tmp\PioneerBDDriveUtility.Language.resx`
- `C:\tmp\-Module-.cs`

## Pioneer Drive Detection

The utility performs a standard SCSI Inquiry and builds a display string from the inquiry data.

It searches detected drives in this order:

1. First drive whose string contains `PIONEER DVD-RW`
2. Otherwise first drive whose string contains `PIONEER`

Relevant code:

- `CDriveInterface.SearchPioneerDrive()`
- `CDriveInterface.ASPI_Inquiry()`
- `CDriveIfSptd.ASPISearch()`

This means the application is not restricted to a hardcoded model list. It accepts Pioneer drives based on the inquiry/vendor string, then checks feature support dynamically.

## Windows / SCSI Transport

The low-level transport uses Windows SCSI pass-through direct.

Drive opening:

```text
\\.\C:
\\.\D:
...
\\.\Z:
```

Important `DeviceIoControl` values:

```text
315412   IOCTL_SCSI_PASS_THROUGH_DIRECT
266264   IOCTL_SCSI_GET_ADDRESS
2951492  used to disable autoplay/media insertion behavior
```

The SCSI pass-through timeout is set to:

```text
108000
```

The utility records command execution time with `Stopwatch`.

## Standard SCSI/MMC Commands Used

The utility wraps many standard optical-drive commands:

```text
0x00 TEST UNIT READY
0x03 REQUEST SENSE, indirectly through pass-through status/sense area
0x04 FORMAT UNIT
0x12 INQUIRY
0x1B START STOP UNIT
0x1E PREVENT / ALLOW MEDIUM REMOVAL
0x23 READ FORMAT CAPACITIES
0x35 SYNCHRONIZE CACHE
0x3B WRITE BUFFER
0x3C READ BUFFER
0x43 READ TOC/PMA/ATIP
0x46 GET CONFIGURATION
0x4A GET EVENT STATUS NOTIFICATION
0x51 READ DISC INFORMATION
0x52 READ TRACK INFORMATION
0x54 SEND OPC INFORMATION
0x55 MODE SELECT(10)
0x5A MODE SENSE(10)
0xAA WRITE(12)
0xAC GET PERFORMANCE
0xAD READ DISC STRUCTURE
0xB6 SET STREAMING
0xBB SET CD SPEED, used here for Pioneer quiet/speed mode control
```

## Pioneer Vendor Buffer Pattern

Most Pioneer-specific features use SCSI `READ BUFFER` / `WRITE BUFFER`.

Generic command wrappers:

```text
READ BUFFER:
  opcode 0x3C
  byte 1 = mode
  byte 2 = buffer ID
  bytes 3-5 = offset
  bytes 6-8 = transfer length

WRITE BUFFER:
  opcode 0x3B
  byte 1 = mode
  byte 2 = buffer ID
  bytes 3-5 = offset
  bytes 6-8 = transfer length
```

Important buffer IDs:

```text
0xE0 / 224  MC direct command/result
0xE1 / 225  MC direct command/result variant
0xE6 / 230  bus power, force eject, CD error measurement
0xF1 / 241  hardware version block
0xF4 / 244  main Pioneer feature/status/capability block
0xF5 / 245  Real Time PureRead status block
0xFA / 250  main Pioneer feature-setting command block
```

## Main Pioneer Capability Block

Most feature detection reads:

```text
READ BUFFER mode=2, buffer ID=244 / 0xF4, offset=0, length=256
```

The utility interprets the returned bytes as follows:

```text
Offset  Meaning
------  -------
2       Advanced Quiet / rotation mode current value, or 0xFF unsupported
3       Fallback quiet mode value
9       PureRead support flag
10      Recording mode support flag
11      Current BD recording mode
13      Current DVD recording mode
16      Peak Power Reducer support
17      Peak Power Reducer current state
20      Smooth tray loading current value, or 0xFF unsupported
22      Drive status support sentinel
23      Drive status; value 1 means speed is limited
24      LED Off current value, or 0xFF unsupported
26      BD-R high-speed recording current value, or 0xFF unsupported
29      Real Time PureRead support flag
41      High-speed data read mode / current rotation indicator
43      Supported-drive marker
44      CD Check support flag
45      Advanced Quiet setting support flag
46      Disc Status support flag
47      USB bus-power check support flag
48      Force eject support flag
49      PureRead version: 1, 2, 3, or 4
50      Custom Eco support flag
52      Select-track inspection support flag
53      Drive type code
54      Switch CD-ROM speed table support flag
55      Fragile/Rental CD mode support flag
56      Fragile/Rental CD mode enabled flag
```

## Supported / Exposed Pioneer Features

The utility exposes the following Pioneer drive features:

- PureRead
- PureRead2
- PureRead3+
- PureRead4+
- Real Time PureRead
- Advanced Quiet Drive Feature
- Persistent Quiet Mode
- Quiet Mode
- Performance Mode
- High-speed data read mode
- Peak Power Reducer
- LED Off
- Smooth tray loading mode
- BD-R high-speed recording mode
- Optimum Writing Speed Feature
- BD/DVD recording mode selection
- Disc Record Surface Quality Mode
- Disc Record Surface Quality Mode High Speed
- Write Quality Mode
- Over Speed Protection Mode
- Entertainment Mode
- High-Performance Mode
- Quality Mode
- Eco Mode
- Custom Eco power timers
- Restore Factory Default
- USB bus-power operation check
- Force eject disc
- Disc status reporting
- Drive status reporting
- CD Check / audio CD quality check
- Select-track CD inspection
- Fragile disc mode for music CD, internally named Rental CD mode

## PureRead

PureRead mode is read from:

```text
READ BUFFER mode=2, buffer ID=0xF4, offset=0, length=256
```

`GetPureReadMode()` reads:

```text
r[4] == 0       PureRead off
r[4] != 0 and r[5] == 254  Perfect mode
r[4] != 0 and r[5] != 254  Master mode
```

PureRead is set by writing to buffer `0xFA`:

```text
WRITE BUFFER mode=1, buffer ID=0xFA, offset=0, length=256
```

Payload:

```text
w[0] = 1       command ID: PureRead
w[1] = 0x80
w[2] = 0       PureRead off
w[2] = 1       PureRead on
w[3] = 8       Master mode
w[4] = 64      Master mode parameter
w[3] = 254     Perfect mode
w[4] = 1       Perfect mode parameter
w[5] = 1       save to EEPROM
w[6] = 1       Real Time PureRead enabled
```

UI meaning:

```text
Master Mode:
  At read error after several retries, interpolated data is returned.

Perfect Mode:
  At read error after several retries, error status is returned.

Real Time PureRead:
  Optimally controls playback so sound is not interrupted while streaming.
```

PureRead version comes from:

```text
0xF4[49]
```

Mapping:

```text
1 = PureRead
2 = PureRead2
3 = PureRead3+
4 = PureRead4+
```

## Real Time PureRead Status

Real Time PureRead information is read from:

```text
READ BUFFER mode=2, buffer ID=245 / 0xF5, offset=0, length=32
```

Returned fields:

```text
r[0..3]   numberOfErrorSector
r[4..7]   numberOfPlaySector
r[8..11]  current/last LBA
```

The UI displays:

- Last transferred time
- Number of transfer sectors
- Error indicator

LBA is converted to MSF format using 75 frames per second.

## Advanced Quiet Drive / Speed Modes

Quiet and speed behavior is controlled through `SET CD SPEED` opcode `0xBB`.

The application sets CDB byte 10 to:

```text
0x80 | mode
```

If saving to EEPROM:

```text
0xC0 | mode
```

Mode mapping in `SetSeionMode()`:

```text
Persistent Quiet Mode  mode 3
Quiet Mode             mode 2
Performance Mode       mode 1
Other/default          mode 0
```

Full byte values without EEPROM save:

```text
Persistent Quiet Mode  0x83
Quiet Mode             0x82
Performance Mode       0x81
Default/other          0x80
```

Full byte values with EEPROM save:

```text
Persistent Quiet Mode  0xC3
Quiet Mode             0xC2
Performance Mode       0xC1
Default/other          0xC0
```

The UI descriptions:

```text
Persistent Quiet Mode:
  The drive will keep spinning in low speed to avoid operational and fan noise.

Quiet Mode:
  Quietness priority mode; the drive adjusts disc speed by the situation.

Performance Mode:
  Performance priority mode; the drive adjusts disc speed by the situation.
```

## High-Speed Data Read

Set through buffer `0xFA` command ID `8`:

```text
w[0] = 8
w[1] = 0
w[2] = 1 if enabled
w[3] = 1 if saving to EEPROM
```

Feature support is checked with:

```text
0xF4[41] != 0xFF
```

## Main Feature-Setting Commands

Most feature settings write to:

```text
WRITE BUFFER mode=1, buffer ID=250 / 0xFA, offset=0, length=256
```

Command IDs in `w[0]`:

```text
ID   Feature
--   -------
1    PureRead / Real Time PureRead
8    High-speed data read
9    Recording mode
10   Peak Power Reducer
11   Tray Smooth / VideoAudio mode
12   LED Off
13   BD-R high-speed recording
14   LED mode
17   Save Custom Eco value to EEPROM
18   Fragile/Rental CD mode
```

Typical boolean payload:

```text
w[0] = command ID
w[1] = 0
w[2] = 1 if enabled
w[3] = 1 if saving to EEPROM
```

## Peak Power Reducer

Support:

```text
0xF4[16] == 1
```

Current state:

```text
0xF4[17] == 1
```

Set command:

```text
w[0] = 10
w[2] = 1 enabled
w[3] = 1 save to EEPROM
```

UI tooltip:

```text
This function is in order to reduce the peak power during the drive operations.
```

## LED Off

Support/current state:

```text
0xF4[24] != 0xFF means supported
0xF4[24] == 1 means LED Off enabled
```

Set command:

```text
w[0] = 12
w[2] = 1 LED off
w[3] = 1 save to EEPROM
```

UI tooltip:

```text
The drive turns off the LED when watching movie titles or playing audio CD.
```

## Smooth Tray Loading

Support/current state:

```text
0xF4[20] != 0xFF means supported
0xF4[20] == 1 means enabled
```

Set command:

```text
w[0] = 11
w[2] = 1 enabled
w[3] = 1 save to EEPROM
```

The UI calls this:

```text
The smooth tray loading mode
```

## BD-R High-Speed Recording Mode

Support/current state:

```text
0xF4[26] != 0xFF means supported
0xF4[26] == 1 means enabled
```

Set command:

```text
w[0] = 13
w[2] = 1 enabled
w[3] = 1 save to EEPROM
```

UI tooltip:

```text
The max write speed using supported disc will be BD-R (SL):16x, BD-R (DL):14x.
```

## Recording Mode / Optimum Writing Speed

Support:

```text
0xF4[10] == 1
```

Current modes:

```text
0xF4[11] = BD recording mode
0xF4[13] = DVD recording mode
```

Set command:

```text
w[0] = 9
w[1] = 0
w[2] = BD mode
w[3] = DVD mode
w[4] = 1 save to EEPROM
```

Mode values:

```text
0 = Off / no optimum writing mode
1 = Disc Record Surface Quality Mode
2 = Write Quality Mode ON
3 = Over Speed Protection Mode
4 = Disc Record Surface Quality Mode High Speed
```

UI tooltips:

```text
Write Quality Mode:
  Write speed will be adjusted to ensure the write quality.

Disc Record Surface Quality Mode:
  Write speed will be adjusted to prioritize the record surface quality.

Disc Record Surface Quality Mode High Speed:
  Write speed will be adjusted to prioritize the record surface quality.
  It is possible to record higher than the conventional.

Over Speed Protection Mode:
  The drive will write within a speed supported by the media manufacture.
```

## Entertainment Mode

The UI calls this **Entertainment Mode** and describes it as:

```text
To set the mode to be optimized for watching the movie title and playing the audio.
```

When selected, the code applies a bundle of settings:

- Disable high-speed recording if supported
- Enable Peak Power Reducer if supported
- Enable LED Off if supported
- Enable Tray Smooth if supported
- Disable high-speed data read if supported
- Set quiet mode / dummy quiet mode
- Configure PureRead depending on support and hardware version

## High-Performance Mode

The UI calls this **High-Performance Mode** and describes it as:

```text
To set the mode to prioritize the speed of writing and reading.
```

Depending on supported features, it:

- Disables Peak Power Reducer
- Disables Tray Smooth
- Enables BD-R high-speed recording if supported
- Sets recording mode off/default
- Enables high-speed data read if supported
- Sets Performance/Quiet-related mode
- Disables Real Time PureRead / PureRead in some cases
- Adjusts Custom Eco timers when supported

## Quality Mode

The UI calls this **Quality Mode** and describes it as:

```text
To set the mode to prioritize the quality of writing and reading.
```

Depending on supported features, it:

- Disables Peak Power Reducer
- Disables Tray Smooth
- Enables high-speed recording if supported
- Sets recording mode to quality-focused values
- Disables high-speed data read
- Sets quiet mode
- Enables PureRead Master mode

## Eco Mode

The UI calls this **Eco Mode** and describes it as:

```text
To set the mode to reduce the power consumption.
```

Depending on supported features, it:

- Enables Peak Power Reducer
- Disables Tray Smooth
- Disables high-speed recording
- Disables high-speed data read
- Uses quiet/low-power settings
- Adjusts Custom Eco timers when supported

## Custom Eco

Support:

```text
0xF4[50] == 1
```

Drive type:

```text
0xF4[53]
```

Special drive type codes:

```text
0    clamshell/external style
224  half-height / HH table
228  half-height / HH table
237  slot-loading table
```

Default UI trackbar value:

```text
Drive type 0    -> 0
Drive type 224  -> 0
Drive type 228  -> 5
Drive type 237  -> 0
```

Custom Eco writes idle/standby timers using `MODE SELECT(10)` page `0x1A`.

Payload used before `MODE SELECT`:

```text
w[1]  = 18
w[8]  = 26
w[9]  = 10
w[11] = 3
w[12..15] = idle timer, big-endian
w[16..19] = standby timer, big-endian
```

The utility reads current timers using:

```text
MODE SENSE(10), page 0x1A
idle timer    = r[12..15]
standby timer = r[16..19]
```

The values passed to the drive are generally UI timer values multiplied by 10.

## Factory Reset Behavior

Factory reset does not appear to send a single factory-reset vendor command. Instead it writes a bundle of default settings:

- Sets quiet/speed mode to default
- Sets PureRead depending on drive support and hardware version
- For non-slim drives:
  - Disables Video/Audio mode
  - Disables Peak Power Reducer if supported
  - Disables LED Off if supported
  - Configures high-speed recording depending on hardware version
  - Clears recording mode if supported
  - Disables high-speed data read if supported
  - Disables Tray Smooth if supported

Hardware version is read from:

```text
READ BUFFER mode=2, buffer ID=241 / 0xF1, offset=0, length=48
```

The code parses ASCII hex at response offset 20.

## USB Bus-Power Check

Support:

```text
0xF4[47] == 1
```

Start check:

```text
WRITE BUFFER mode=2, buffer ID=230 / 0xE6, offset=65536 / 0x10000, length=0
```

Read result:

```text
READ BUFFER mode=2, buffer ID=230 / 0xE6, offset=65536 / 0x10000, length=8
```

The result is a 32-bit big-endian value from response bytes:

```text
r[4..7]
```

## Force Eject

Support:

```text
0xF4[48] == 1
```

Force eject command:

```text
WRITE BUFFER mode=2, buffer ID=230 / 0xE6, offset=131072 / 0x20000, length=0
```

UI tooltip:

```text
When a disc cannot be taken out, it will be ejected forcibly.
```

Normal eject uses `START STOP UNIT` with LoEj set.

## Fragile Disc Mode / Rental CD Mode

The UI calls this:

```text
Fragile disc mode for music CD
```

Internal code calls it:

```text
RentalCDMode
```

Support:

```text
0xF4[55] != 0
```

Current enabled state:

```text
0xF4[56] != 0
```

Set command:

```text
w[0] = 18
w[1] = 0
w[2] = 1 save to EEPROM
w[3] = 1 off/disable, according to SetRentalCDMode(off, eepSave)
```

UI description:

```text
Slows down disc rotation speed to reduce force on the discs.
This function works when reading Audio CDs.
```

Tooltip:

```text
This function does not work with CD-R and CD-RW.
```

## Disc Status / Drive Status

Disc status support:

```text
0xF4[46] == 1
```

Drive status support:

```text
0xF4[22] != 0xFF
```

Drive status value:

```text
0xF4[23]
```

If drive status is `1`, the UI displays:

```text
The disc or temp, the drive limits the read/write speed.
```

Disc status uses standard `READ DISC INFORMATION`:

```text
r[2] & 3
```

Mapping:

```text
0 = Blank Disc
1 = Writable Media
2 = Finalized Disc or writable depending on media/format
other = Cannot Write
```

## Disc Type / Media Code Mapping

The utility obtains a media code with a vendor direct command:

```text
WRITE BUFFER mode=2, buffer ID=225 / 0xE1, offset=0, length=32
READ BUFFER  mode=2, buffer ID=225 / 0xE1, offset=0, length=32

w[0] = 145 / 0x91
w[1] = 64 / 0x40

mediaCode = (r[10] << 8) | r[6]
```

Visible disc type mapping by low byte:

```text
0x00  CD-ROM
0x20  CD-R
0x10  CD-RW
0x40  DVD-ROM Single
0x44  DVD-ROM Dual
0x46  DVD-ROM Dual
0x50  DVD-RW
0x60  DVD-R Single
0x68  DVD-R 3.95GB Single
0x51  DVD+RW
0x61  DVD+R Single
0x66  DVD-R Dual
0x67  DVD+R Dual
0x70  DVD-RAM
0x80  BD-ROM Single
0x86  BD-ROM Dual
0x8A  BD-ROM Triple
0x90  BD-RE Single
0x96  BD-RE Dual
0x9A  BD-RE Triple
0xA0  BD-R Single
0xA6  BD-R Dual
0xAA  BD-R Triple
0xAE  BD-R Quadruple
else  Unknown Disc
```

Media family mapping used for media ID extraction:

```text
0  CD-ROM
1  DVD-ROM
2  DVD-R / DVD-RW family
3  DVD+R / DVD+RW family
4  DVD-RAM
5  BD-R / BD-RE family
6  CD-R / CD-RW family
7  BD-ROM
```

## Media ID Extraction

Media IDs are extracted differently by media family:

```text
BD-R / BD-RE:
  READ DISC STRUCTURE mediaType=1, format=0
  ASCII from response offset 104, length 9

DVD-R / DVD-RW:
  READ DISC STRUCTURE mediaType=0, format=14
  ASCII from response offsets around 21, skipping bytes 6 and 7

DVD+R / DVD+RW:
  READ DISC STRUCTURE mediaType=0, format=0
  ASCII from response offset 23, length 11

CD-R / CD-RW:
  READ TOC format=4
  Displays ATIP-like time fields as:
  MM:SS:FF-MM:SS:FF

Default/other DVD-ish handling:
  READ DISC STRUCTURE mediaType=0, format=0
  ASCII from response offset 601, up to 16 bytes or carriage return
```

Zero bytes are converted to underscores. A helper later turns trailing underscores into spaces.

## Write Protection

The utility checks write protection using `READ DISC STRUCTURE` format `0xC0`.

For BD-like media codes in the `0x90` or `0xA0` high nibble, `mediaType=1`; otherwise `mediaType=0`.

Check:

```text
(r[4] & 0x0F) != 0 means write protected
```

## CD Check / Audio CD Quality Check

Support:

```text
0xF4[44] == 1
```

Select-track inspection support:

```text
0xF4[52] == 1
```

The utility can inspect audio CD tracks and display a result. It reads TOC/track data, then starts error-rate measurement.

Start/stop measurement:

```text
WRITE BUFFER mode=2, buffer ID=230 / 0xE6, offset=3145728 / 0x300000, length=32
```

Payload:

```text
w[0] = 0xFF
w[1] = 2
w[2] = 0 start, 1 stop
w[3..6] = start address, big-endian
w[11..14] = unit size, big-endian
```

Read measurement result:

```text
READ BUFFER mode=2, buffer ID=230 / 0xE6, offset=3145728 / 0x300000, length=64
```

Returned measurement fields:

```text
r[4..5]    C1 uncorrectable frames count
r[14..15]  C2 uncorrectable bytes
r[18..21]  end address
r[22..25]  validity check; 0xFFFFFFFF means error data invalid
r[60..61]  TE peak
r[62..63]  TE integration max
```

The UI text says:

```text
75 sectors is the amount of data for one second.
One frame is 1/75 second.
```

CD Check result language:

```text
Normal condition:
  Some part is unable to read smoothly, though the disc remains playable
  as original sound in most CD players.

Low condition:
  The disc remains playable in most CD players, though its data may be
  incorporated. PureRead can help recover original sound by duplicating
  the disc.

Bad condition:
  The disc might not be played back in some CD players.
```

## Drive Serial Number

The utility reads the drive serial number with:

```text
GET CONFIGURATION requestedType=0, startFeatureNumber=264 / 0x0108
```

It copies 12 bytes from response offset 12:

```text
r[12..23]
```

Then decodes ASCII.

## Hardware Version

Read with:

```text
READ BUFFER mode=2, buffer ID=241 / 0xF1, offset=0, length=48
```

The utility parses ASCII hex from response offset 20.

The code compares hardware version against:

```text
36864 decimal = 0x9000
```

Some defaults differ before/after that threshold.

## Current Power Status

The code reads power status with:

```text
GET EVENT STATUS NOTIFICATION notificationClassRequest=4
```

It returns:

```text
r[5]
```

This is used to prevent switching some PureRead/CD-ROM speed-table settings while the disc is active.

## Supported Drive Marker

The utility checks whether the current Pioneer drive is supported with:

```text
READ BUFFER mode=2, buffer ID=0xF4, offset=0, length=256
0xF4[43] == 1
```

This is probably the most important dynamic support flag.

## Registry Keys

User preferences are stored under:

```text
HKCU\SOFTWARE\Pioneer BD Drive Utility
```

Known values:

```text
FwUpdateTipOFF
RentalCDmodeOnTipOFF
RentalCDmodeON
```

## Important Takeaway

The most useful information in this code is not a model list. It is a map of Pioneer BD-drive vendor features:

- The utility accepts Pioneer drives by inquiry string.
- It then reads a Pioneer capability block at `READ BUFFER 0xF4`.
- It uses individual bytes in that block to decide which features to show.
- It writes feature changes through `WRITE BUFFER 0xFA` command IDs.
- Several utility functions use `WRITE/READ BUFFER 0xE6` at special offsets.

This gives a practical starting point for reimplementing or probing Pioneer drive features without relying on the original utility UI.

