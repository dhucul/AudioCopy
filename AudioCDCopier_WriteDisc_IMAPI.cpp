#define NOMINMAX
#include "AudioCDCopier.h"
#include "ConsoleColors.h"
#include "WriteDiscInternal.h"
#include <comdef.h>
#include <imapi2.h>
#include <imapi2error.h>
#include <fstream>
#include <iostream>
#include <vector>
#include <windows.h>
#include <wrl/client.h>

#pragma comment(lib, "ole32.lib")

using Microsoft::WRL::ComPtr;

// ============================================================================
// Helper: Find IMAPI recorder matching a drive letter
// ============================================================================
static bool FindRecorderForDrive(wchar_t driveLetter,
	ComPtr<IDiscRecorder2>& recorder) {

	ComPtr<IDiscMaster2> master;
	HRESULT hr = CoCreateInstance(__uuidof(MsftDiscMaster2), nullptr,
		CLSCTX_ALL, IID_PPV_ARGS(&master));
	if (FAILED(hr)) return false;

	LONG count = 0;
	master->get_Count(&count);

	for (LONG i = 0; i < count; i++) {
		BSTR uid = nullptr;
		if (FAILED(master->get_Item(i, &uid))) continue;

		ComPtr<IDiscRecorder2> rec;
		hr = CoCreateInstance(__uuidof(MsftDiscRecorder2), nullptr,
			CLSCTX_ALL, IID_PPV_ARGS(&rec));
		if (FAILED(hr)) { SysFreeString(uid); continue; }

		hr = rec->InitializeDiscRecorder(uid);
		SysFreeString(uid);
		if (FAILED(hr)) continue;

		SAFEARRAY* mountPoints = nullptr;
		if (SUCCEEDED(rec->get_VolumePathNames(&mountPoints)) && mountPoints) {
			LONG lb = 0, ub = 0;
			SafeArrayGetLBound(mountPoints, 1, &lb);
			SafeArrayGetUBound(mountPoints, 1, &ub);
			for (LONG j = lb; j <= ub; j++) {
				BSTR path = nullptr;
				SafeArrayGetElement(mountPoints, &j, &path);
				if (path && towupper(path[0]) == towupper(driveLetter)) {
					SysFreeString(path);
					SafeArrayDestroy(mountPoints);
					recorder = rec;
					return true;
				}
				SysFreeString(path);
			}
			SafeArrayDestroy(mountPoints);
		}
	}
	return false;
}

// ============================================================================
// Helper: Create IStream by streaming file chunks (avoids full RAM copy)
// ============================================================================
static HRESULT CreateStreamFromFileRange(const std::wstring& filePath,
	long long offset, DWORD length, IStream** ppStream) {

	HRESULT hr = CreateStreamOnHGlobal(nullptr, TRUE, ppStream);
	if (FAILED(hr)) return hr;

	std::ifstream file(filePath, std::ios::binary);
	if (!file.is_open()) return E_FAIL;

	file.seekg(offset);

	constexpr DWORD CHUNK = 256 * 1024;
	std::vector<BYTE> buf(CHUNK);
	DWORD remaining = length;
	while (remaining > 0 && file.good()) {
		DWORD toRead = (remaining < CHUNK) ? remaining : CHUNK;
		file.read(reinterpret_cast<char*>(buf.data()), toRead);
		DWORD got = static_cast<DWORD>(file.gcount());
		if (got == 0) break;

		ULONG written = 0;
		hr = (*ppStream)->Write(buf.data(), got, &written);
		if (FAILED(hr)) return hr;
		remaining -= got;
	}

	LARGE_INTEGER zero = {};
	(*ppStream)->Seek(zero, STREAM_SEEK_SET, nullptr);
	return S_OK;
}

// ============================================================================
// Helper: Build a DAO stream from .bin with 150-frame pregap prepended
// ============================================================================
static HRESULT CreateDAOStream(const std::wstring& binFile,
	DWORD totalSectors, IStream** ppStream) {

	HRESULT hr = CreateStreamOnHGlobal(nullptr, TRUE, ppStream);
	if (FAILED(hr)) return hr;

	// Write 150 frames (2 seconds) of silence for the mandatory pregap
	constexpr DWORD PREGAP_SECTORS = 150;
	std::vector<BYTE> silence(AUDIO_SECTOR_SIZE, 0x00);
	for (DWORD i = 0; i < PREGAP_SECTORS; i++) {
		ULONG written = 0;
		hr = (*ppStream)->Write(silence.data(), AUDIO_SECTOR_SIZE, &written);
		if (FAILED(hr)) return hr;
	}

	// Stream the audio data from .bin in chunks
	std::ifstream bin(binFile, std::ios::binary);
	if (!bin.is_open()) return E_FAIL;

	constexpr DWORD CHUNK = 256 * 1024;
	std::vector<BYTE> buf(CHUNK);
	long long remaining = static_cast<long long>(totalSectors) * AUDIO_SECTOR_SIZE;
	while (remaining > 0 && bin.good()) {
		DWORD toRead = (remaining < CHUNK) ? static_cast<DWORD>(remaining) : CHUNK;
		bin.read(reinterpret_cast<char*>(buf.data()), toRead);
		DWORD got = static_cast<DWORD>(bin.gcount());
		if (got == 0) break;

		ULONG written = 0;
		hr = (*ppStream)->Write(buf.data(), got, &written);
		if (FAILED(hr)) return hr;
		remaining -= got;
	}

	LARGE_INTEGER zero = {};
	(*ppStream)->Seek(zero, STREAM_SEEK_SET, nullptr);
	return S_OK;
}

// ============================================================================
// WriteDiscIMAPI - IMAPI2 fallback when raw SCSI CUE SHEET is rejected
// ============================================================================
bool AudioCDCopier::WriteDiscIMAPI(const std::wstring& binFile,
	const std::vector<TrackWriteInfo>& tracks,
	DWORD totalSectors, int speed) {

	Console::BoxHeading("IMAPI2 Fallback Write");
	Console::Info("Using Microsoft IMAPI2 API (drive rejected raw SCSI layout)\n");

	// ── Resolve drive letter from m_drive before closing it ─────────
	wchar_t driveLetter = L'\0';
	for (wchar_t c = L'A'; c <= L'Z'; c++) {
		ScsiDrive probe;
		if (!probe.Open(c)) continue;

		std::string vendor, model, ourVendor, ourModel;
		bool probeOk = probe.GetDriveInfo(vendor, model);
		bool oursOk = m_drive.GetDriveInfo(ourVendor, ourModel);
		probe.Close();

		if (probeOk && oursOk && vendor == ourVendor && model == ourModel) {
			driveLetter = c;
			break;
		}
	}

	if (driveLetter == L'\0') {
		Console::Error("Cannot identify drive letter for IMAPI2\n");
		return false;
	}

	Console::Info("Using drive ");
	std::wcout << driveLetter << L":\n";

	// Close SCSI handle so IMAPI2 can get exclusive access
	m_drive.Close();

	// Initialize COM
	HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
	bool comOwner = SUCCEEDED(hr);
	if (hr == RPC_E_CHANGED_MODE) {
		hr = S_OK;
	}
	if (FAILED(hr)) {
		Console::Error("COM initialization failed\n");
		m_drive.Open(driveLetter);
		return false;
	}

	auto cleanup = [&](bool reopenDrive) {
		if (comOwner) CoUninitialize();
		if (reopenDrive) m_drive.Open(driveLetter);
	};

	ComPtr<IDiscRecorder2> recorder;
	if (!FindRecorderForDrive(driveLetter, recorder)) {
		Console::Error("IMAPI2 cannot find disc recorder for drive ");
		std::wcout << driveLetter << L":\n";
		cleanup(true);
		return false;
	}

	// ── Attempt 1: DAO via IDiscFormat2RawCD (preserves 1:1 layout) ─
	Console::Info("Trying IMAPI2 Disc-At-Once (1:1 faithful)...\n");
	{
		ComPtr<IDiscFormat2RawCD> rawCD;
		hr = CoCreateInstance(__uuidof(MsftDiscFormat2RawCD), nullptr,
			CLSCTX_ALL, IID_PPV_ARGS(&rawCD));

		if (SUCCEEDED(hr)) {
			VARIANT_BOOL supported = VARIANT_FALSE;
			rawCD->IsCurrentMediaSupported(recorder.Get(), &supported);

			if (supported == VARIANT_TRUE) {
				hr = rawCD->put_Recorder(recorder.Get());
				if (SUCCEEDED(hr)) {
					// Audio sectors: 2352 bytes, no subchannel
					rawCD->put_RequestedSectorType(
						IMAPI_FORMAT2_RAW_CD_SUBCODE_IS_COOKED);

					LONG sectorsPerSecond = static_cast<LONG>(speed) * 75;
					rawCD->SetWriteSpeed(sectorsPerSecond, VARIANT_FALSE);

					hr = rawCD->PrepareMedia();
					if (SUCCEEDED(hr)) {
						Console::Info("Building DAO stream (150 pregap + ");
						std::cout << totalSectors << " audio sectors)...\n";

						ComPtr<IStream> daoStream;
						hr = CreateDAOStream(binFile, totalSectors, &daoStream);
						if (SUCCEEDED(hr)) {
							Console::Info("Writing disc via IMAPI2 DAO...\n");
							hr = rawCD->WriteMedia(daoStream.Get());
							if (SUCCEEDED(hr)) {
								hr = rawCD->ReleaseMedia();
								if (FAILED(hr)) {
									Console::Warning("ReleaseMedia warning (HRESULT: ");
									std::cout << std::hex << hr << std::dec << ")\n";
								}
								Console::Success("IMAPI2 DAO write completed (1:1 faithful)\n");
								cleanup(true);
								return true;
							}
							Console::Warning("IMAPI2 DAO WriteMedia failed (HRESULT: ");
							std::cout << std::hex << hr << std::dec << ")\n";
						}
						rawCD->ReleaseMedia();
					}
				}
			}
		}
		Console::Info("DAO not available on this drive/media — trying TAO...\n");
	}

	// ── Attempt 2: TAO fallback (functional but adds 2-sec gaps) ────
	Console::Warning("Track-At-Once mode: inter-track gaps will be 2 seconds\n");
	Console::Warning("This will NOT produce a 1:1 copy of the original disc\n");

	ComPtr<IDiscFormat2TrackAtOnce> tao;
	hr = CoCreateInstance(__uuidof(MsftDiscFormat2TrackAtOnce), nullptr,
		CLSCTX_ALL, IID_PPV_ARGS(&tao));
	if (FAILED(hr)) {
		Console::Error("Cannot create IMAPI2 TAO writer (HRESULT: ");
		std::cout << std::hex << hr << std::dec << ")\n";
		cleanup(true);
		return false;
	}

	VARIANT_BOOL supported = VARIANT_FALSE;
	hr = tao->IsCurrentMediaSupported(recorder.Get(), &supported);
	if (FAILED(hr) || supported == VARIANT_FALSE) {
		Console::Error("Current media not supported by IMAPI2\n");
		cleanup(true);
		return false;
	}

	hr = tao->put_Recorder(recorder.Get());
	if (FAILED(hr)) {
		Console::Error("Cannot assign recorder (HRESULT: ");
		std::cout << std::hex << hr << std::dec << ")\n";
		cleanup(true);
		return false;
	}

	LONG sectorsPerSecond = static_cast<LONG>(speed) * 75;
	tao->SetWriteSpeed(sectorsPerSecond, VARIANT_FALSE);

	hr = tao->PrepareMedia();
	if (FAILED(hr)) {
		Console::Error("IMAPI2 TAO PrepareMedia failed (HRESULT: ");
		std::cout << std::hex << hr << std::dec << ")\n";
		cleanup(true);
		return false;
	}

	Console::Info("Writing ");
	std::cout << tracks.size() << " tracks via IMAPI2 Track-At-Once...\n";

	for (size_t i = 0; i < tracks.size(); i++) {
		const auto& t = tracks[i];
		DWORD trackSectors = t.endLBA - t.startLBA + 1;
		DWORD trackBytes = trackSectors * AUDIO_SECTOR_SIZE;
		long long fileOffset = static_cast<long long>(t.startLBA) * AUDIO_SECTOR_SIZE;

		Console::Info("  Track ");
		std::cout << t.trackNumber << " (" << trackSectors << " sectors, "
			<< (trackBytes / (1024 * 1024)) << " MB)...\n";

		ComPtr<IStream> stream;
		hr = CreateStreamFromFileRange(binFile, fileOffset, trackBytes, &stream);
		if (FAILED(hr)) {
			Console::Error("Cannot create stream for track ");
			std::cout << t.trackNumber << "\n";
			tao->ReleaseMedia();
			cleanup(true);
			return false;
		}

		hr = tao->AddAudioTrack(stream.Get());
		if (FAILED(hr)) {
			Console::Error("IMAPI2 AddAudioTrack failed for track ");
			std::cout << t.trackNumber << " (HRESULT: "
				<< std::hex << hr << std::dec << ")\n";
			tao->ReleaseMedia();
			cleanup(true);
			return false;
		}

		Console::Success("  Track ");
		std::cout << t.trackNumber << " written\n";
	}

	hr = tao->ReleaseMedia();
	if (FAILED(hr)) {
		Console::Warning("IMAPI2 ReleaseMedia warning (HRESULT: ");
		std::cout << std::hex << hr << std::dec << ")\n";
	}

	Console::Success("IMAPI2 TAO write completed successfully\n");
	Console::Warning("Note: inter-track gaps are 2 seconds (not original layout)\n");
	cleanup(true);
	return true;
}