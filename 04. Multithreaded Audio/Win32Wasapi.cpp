#include "Win32Wasapi.h"

#include <assert.h>

#include <avrt.h>
#include <audioclient.h>
#include <mmdeviceapi.h>

// TODO: what's missing here:
// * proper error handling, like when no audio device is present (currently asserts)
// * automatically switch to new device when default audio device changes (IMMNotificationClient)

#pragma comment (lib, "avrt")
#pragma comment (lib, "ole32")
#pragma comment (lib, "onecore")

void WA__Lock(WasapiAudio* audio)
{
	// Try to toggle audio->lock from FALSE to TRUE
	while (InterlockedCompareExchange(&audio->lock, TRUE, FALSE) != FALSE)
	{
		// It was already TRUE, wait until whoever's locked it to wake us
		LONG locked = FALSE;
		WaitOnAddress(&audio->lock, &locked, sizeof(locked), INFINITE);
	}
	// Now audio->lock == TRUE
}

void WA__Unlock(WasapiAudio* audio)
{
	// Set audio->lock to FALSE
	InterlockedExchange(&audio->lock, FALSE);
	// Wake any threads waiting on lock
	WakeByAddressSingle(&audio->lock);
}

static DWORD CALLBACK WA__AudioThread(LPVOID arg)
{
	WasapiAudio* audio = (WasapiAudio*)arg;

	DWORD task = 0;
	HANDLE handle = AvSetMmThreadCharacteristicsW(L"Pro Audio", &task);
	assert(handle);

	IAudioClient* client = audio->client;

	IAudioRenderClient* playback;
	HRESULT hr = client->GetService(__uuidof(IAudioRenderClient), (LPVOID*)&playback);
    assert(SUCCEEDED(hr));

	// get audio buffer size in samples
	UINT32 bufferSamples;
	hr = client->GetBufferSize(&bufferSamples);
    assert(SUCCEEDED(hr));

	// start the playback
	hr = client->Start();
    assert(SUCCEEDED(hr));

	UINT32 bytesPerSample = audio->bufferFormat->nBlockAlign;
	UINT32 rbMask = audio->rbSize - 1;
	BYTE* input = audio->buffer1;

	while (WaitForSingleObject(audio->event, INFINITE) == WAIT_OBJECT_0)
	{
		if (InterlockedExchange(&audio->stop, FALSE))
		{
			break;
		}

		UINT32 paddingSamples;
		hr = client->GetCurrentPadding(&paddingSamples);
    	assert(SUCCEEDED(hr));

		// get output buffer from WASAPI
		BYTE* output;
		UINT32 maxOutputSamples = bufferSamples - paddingSamples;
		hr = playback->GetBuffer(maxOutputSamples, &output);
    	assert(SUCCEEDED(hr));

		WA__Lock(audio);

		UINT32 readOffset = audio->rbReadOffset;
		UINT32 writeOffset = audio->rbWriteOffset;

		// how many bytes available to read from ringbuffer
		UINT32 availableSize = writeOffset - readOffset;

		// how many samples available
		UINT32 availableSamples = availableSize / bytesPerSample;

		// will use up to max that's possible to output
		UINT32 useSamples = min(availableSamples, maxOutputSamples);

		// how many bytes to use
		UINT32 useSize = useSamples * bytesPerSample;

		// lock range [read, lock) that memcpy will read from below
		audio->rbLockOffset = readOffset + useSize;

		// will always submit required amount of samples, but if there's not enough to use, then submit silence
		UINT32 submitCount = useSamples ? useSamples : maxOutputSamples;
		DWORD flags = useSamples ? 0 : AUDCLNT_BUFFERFLAGS_SILENT;

		// remember how many samples are submitted
		audio->bufferUsed += submitCount;

		WA__Unlock(audio);

		// copy bytes to output
		// safe to do it outside WA__Lock/Unlock, because nobody will overwrite [read, lock) interval
		memcpy(output, input + (readOffset & rbMask), useSize);

		// advance read offset up to lock position, allows writing to [read, lock) interval
		InterlockedAdd(&audio->rbReadOffset, useSize);

		// submit output buffer to WASAPI
		hr = playback->ReleaseBuffer(submitCount, flags);
    	assert(SUCCEEDED(hr));
	}

	// stop the playback
	hr = client->Stop();
	assert(SUCCEEDED(hr));
	playback->Release();

	AvRevertMmThreadCharacteristics(handle);
	return 0;
}

DWORD RoundUpPow2(DWORD value)
{
	unsigned long index;
	_BitScanReverse(&index, value - 1);
	assert(index < 31);
	return 1U << (index + 1);
}

void Win32AudioStart(WasapiAudio* audio, size_t sampleRate, size_t channelCount, DWORD channelMask)
{
	// initialize COM
	HRESULT hr = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
	assert(SUCCEEDED(hr));

	// create enumerator to get audio device
	IMMDeviceEnumerator* enumerator;
    hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator), (LPVOID*)(&enumerator));
	assert(SUCCEEDED(hr));

	// get default playback device
	IMMDevice* device;
	hr = enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device);
	assert(SUCCEEDED(hr));
	enumerator->Release();

	// create audio client for device
	hr = device->Activate(__uuidof(IAudioClient2), CLSCTX_ALL, NULL, (LPVOID*)&audio->client);
	assert(SUCCEEDED(hr));
	device->Release();

	WAVEFORMATEXTENSIBLE formatEx = {};
	{
		formatEx.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
		formatEx.Format.nChannels = (WORD)channelCount;
		formatEx.Format.nSamplesPerSec = (WORD)sampleRate;
		formatEx.Format.nAvgBytesPerSec = (DWORD)(sampleRate * channelCount * sizeof(float));
		formatEx.Format.nBlockAlign = (WORD)(channelCount * sizeof(float));
		formatEx.Format.wBitsPerSample = (WORD)(8 * sizeof(float));
		formatEx.Format.cbSize = sizeof(formatEx) - sizeof(formatEx.Format);
		formatEx.Samples.wValidBitsPerSample = 8 * sizeof(float);
		formatEx.dwChannelMask = channelMask;
		formatEx.SubFormat = KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;
	}

	WAVEFORMATEX* wfx;
	if (sampleRate == 0 || channelCount == 0 || channelMask == 0)
	{
		// use native mixing format
		hr = audio->client->GetMixFormat(&wfx);
		assert(SUCCEEDED(hr));
		audio->bufferFormat = wfx;
	}
	else
	{
		// will use our format
		wfx = &formatEx.Format;
		audio->bufferFormat = (WAVEFORMATEX*)CoTaskMemAlloc(sizeof(formatEx));
        CopyMemory(audio->bufferFormat, &formatEx, sizeof(formatEx));
	}
	
	BOOL clientInitialized = FALSE;

	// try to initialize client with newer functionality in Windows 10, no AUTOCONVERTPCM allowed
	IAudioClient3* client3;
	if (SUCCEEDED(audio->client->QueryInterface(__uuidof(IAudioClient3), (LPVOID*)&client3)))
	{
		// minimum buffer size will typically be 480 samples (10msec @ 48khz)
		// but it can be 128 samples (2.66 msec @ 48khz) if driver is properly installed
		// see bullet-point instructions here: https://learn.microsoft.com/en-us/windows-hardware/drivers/audio/low-latency-audio#measurement-tools
		UINT32 defaultPeriodSamples, fundamentalPeriodSamples, minPeriodSamples, maxPeriodSamples;
		hr = client3->GetSharedModeEnginePeriod(wfx, &defaultPeriodSamples, &fundamentalPeriodSamples, &minPeriodSamples, &maxPeriodSamples);

		if(SUCCEEDED(hr))
		{
			const DWORD flags = AUDCLNT_STREAMFLAGS_EVENTCALLBACK;
			if (SUCCEEDED(client3->InitializeSharedAudioStream(flags, minPeriodSamples, wfx, NULL)))
			{
				clientInitialized = TRUE;
			}
		}
		client3->Release();
	}

	if (!clientInitialized)
	{
		// get duration for shared-mode streams, this will typically be 480 samples (10msec @ 48khz)
		REFERENCE_TIME duration;
		hr = audio->client->GetDevicePeriod(&duration, NULL);
		assert(SUCCEEDED(hr));

		// initialize audio playback
		const DWORD flags = AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;
		hr = audio->client->Initialize(AUDCLNT_SHAREMODE_SHARED, flags, duration, 0, wfx, NULL);
		assert(SUCCEEDED(hr));
	}

	UINT32 bufferSamples;
	hr = audio->client->GetBufferSize(&bufferSamples);
	assert(SUCCEEDED(hr));
	audio->bufferSize = bufferSamples * audio->bufferFormat->nBlockAlign;

	// setup event handle to wait on
	audio->event = CreateEventW(NULL, FALSE, FALSE, NULL);
	hr = audio->client->SetEventHandle(audio->event);
	assert(SUCCEEDED(hr));

	// use at least 64KB or 1 second whichever is larger, and round upwards to pow2 for ringbuffer
	DWORD rbSize = RoundUpPow2(max(64 * 1024, audio->bufferFormat->nAvgBytesPerSec));

	// reserve virtual address placeholder for 2x size for magic ringbuffer
	char* placeholder1 = (char*)VirtualAlloc2(NULL, NULL, 2 * rbSize, MEM_RESERVE | MEM_RESERVE_PLACEHOLDER, PAGE_NOACCESS, NULL, 0);
	char* placeholder2 = placeholder1 + rbSize;
	assert(placeholder1);

	// split allocated address space in half
	BOOL ok = VirtualFree(placeholder1, rbSize, MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER);
	assert(ok);

	// create page-file backed section for buffer
	HANDLE section = CreateFileMappingW(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0, rbSize, NULL);
	assert(section);

	// map same section into both addresses
	void* view1 = MapViewOfFile3(section, NULL, placeholder1, 0, rbSize, MEM_REPLACE_PLACEHOLDER, PAGE_READWRITE, NULL, 0);
	void* view2 = MapViewOfFile3(section, NULL, placeholder2, 0, rbSize, MEM_REPLACE_PLACEHOLDER, PAGE_READWRITE, NULL, 0);
	assert(view1 && view2);

	// this is ok, actual memory will be freed only when it is unmapped
	VirtualFree(placeholder1, 0, MEM_RELEASE);
	VirtualFree(placeholder2, 0, MEM_RELEASE);
	CloseHandle(section);

	audio->sampleBuffer = NULL;
	audio->sampleCount = 0;
	audio->playCount = 0;
	audio->buffer1 = (BYTE*)view1;
	audio->buffer2 = (BYTE*)view2;
	audio->rbSize = rbSize;
	audio->bufferUsed = 0;
	audio->bufferFirstLock = TRUE;
	audio->rbReadOffset = 0;
	audio->rbLockOffset = 0;
	audio->rbWriteOffset = 0;
	InterlockedExchange(&audio->stop, FALSE);
	InterlockedExchange(&audio->lock, FALSE);
	audio->thread = CreateThread(NULL, 0, &WA__AudioThread, audio, 0, NULL);
}

void Win32AudioStop(WasapiAudio* audio)
{
	// notify thread to stop
	InterlockedExchange(&audio->stop, TRUE);
	SetEvent(audio->event);

	// wait for thread to finish
	WaitForSingleObject(audio->thread, INFINITE);
	CloseHandle(audio->thread);
	CloseHandle(audio->event);

	// release ringbuffer
	UnmapViewOfFileEx(audio->buffer1, 0);
	UnmapViewOfFileEx(audio->buffer2, 0);

	// release audio client
	CoTaskMemFree(audio->bufferFormat);
	audio->client->Release();

	// done with COM
	CoUninitialize();
}

void Win32AudioLockBuffer(WasapiAudio* audio)
{
	UINT32 bytesPerSample = audio->bufferFormat->nBlockAlign;
	UINT32 rbSize = audio->rbSize;
	UINT32 bufferSize = audio->bufferSize;

	WA__Lock(audio);

	UINT32 readOffset = audio->rbReadOffset;
	UINT32 lockOffset = audio->rbLockOffset;
	UINT32 writeOffset = audio->rbWriteOffset;

	// how many bytes are used in buffer by reader = [read, lock) range
	UINT32 usedSize = lockOffset - readOffset;

	// make sure there are samples available for one wasapi buffer submission
	// so in case audio thread needs samples before UnlockBuffer is called, it can get some 
	if (usedSize < bufferSize)
	{
		// how many bytes available in current buffer = [read, write) range
		UINT32 availSize = writeOffset - readOffset;

		// if [read, lock) is smaller than bufferSize buffer, then increase lock to [read, read+bufferSize) range
		usedSize = min(bufferSize, availSize);
		audio->rbLockOffset = lockOffset = readOffset + usedSize;
	}

	// how many bytes can be written to buffer
	UINT32 writeSize = rbSize - usedSize;

	// reset write marker to beginning of lock offset (can start writing there)
	audio->rbWriteOffset = lockOffset;

	// reset play sample count, use 0 for playCount when LockBuffer is called first time
	audio->playCount = audio->bufferFirstLock ? 0 : audio->bufferUsed;
	audio->bufferFirstLock = FALSE;
	audio->bufferUsed = 0;

	WA__Unlock(audio);

	// buffer offset/size where to write
	// safe to write in [write, read) range, because reading happen in [read, lock) range (lock==write)
	audio->sampleBuffer = audio->buffer1 + (lockOffset & (rbSize - 1));
	audio->sampleCount = writeSize / bytesPerSample;
}

void Win32AudioUnlockBuffer(WasapiAudio* audio, size_t writtenSamples)
{
	UINT32 bytesPerSample = audio->bufferFormat->nBlockAlign;
	size_t writeSize = writtenSamples * bytesPerSample;

	// advance write offset to allow reading new samples
	InterlockedAdd(&audio->rbWriteOffset, (LONG)writeSize);
}
