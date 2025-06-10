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

	IAudioRenderClient* renderClient;
	HRESULT hr = client->GetService(__uuidof(IAudioRenderClient), (LPVOID*)&renderClient);
    assert(SUCCEEDED(hr));

	// get audio buffer size in samples
	UINT32 bufferSamples;
	hr = client->GetBufferSize(&bufferSamples);
    assert(SUCCEEDED(hr));

	// start the playback
	hr = client->Start();
    assert(SUCCEEDED(hr));

	UINT32 bytesPerSample = audio->bufferFormat->nBlockAlign;
	UINT32 rbMask = audio->ringBufferSize - 1;
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
		hr = renderClient->GetBuffer(maxOutputSamples, &output);
    	assert(SUCCEEDED(hr));

		WA__Lock(audio);

		UINT32 readOffset = audio->rbReadOffset;
		UINT32 writeOffset = audio->rbWriteOffset;

		// how many bytes available to read from ringbuffer
		UINT32 availableSize = writeOffset - readOffset;
		UINT32 numSamplesAvailable = availableSize / bytesPerSample;

		// will use up to max that's possible to output
		UINT32 numSamplesToSubmit = min(numSamplesAvailable, maxOutputSamples);

		// how many bytes we will read from ringbuffer
		UINT32 numBytesToRead = numSamplesToSubmit * bytesPerSample;

		// lock the range [read, lock) we will be reading
		// so the main thread cannot overwrite it.
		audio->rbLockOffset = readOffset + numBytesToRead;
		
		DWORD flags = 0;
		// If we have no samples to submit, fill buffer with silence
		if (numSamplesToSubmit == 0)
		{
			numSamplesToSubmit = maxOutputSamples;
			flags = AUDCLNT_BUFFERFLAGS_SILENT;
		}

		// remember how many samples are submitted
		audio->numSamplesSubmittedSinceLastTick += numSamplesToSubmit;

		WA__Unlock(audio);

		// copy bytes to output
		// safe to do it outside WA__Lock/Unlock, because nobody will overwrite [read, lock) interval
		memcpy(output, input + (readOffset & rbMask), numBytesToRead);

		// advance read offset up to lock position, allows writing to [read, lock) interval
		InterlockedAdd(&audio->rbReadOffset, numBytesToRead);

		// submit output buffer to WASAPI
		hr = renderClient->ReleaseBuffer(numSamplesToSubmit, flags);
    	assert(SUCCEEDED(hr));
	}

	// stop the playback
	hr = client->Stop();
	assert(SUCCEEDED(hr));
	renderClient->Release();

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
	*audio = {};

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
	audio->outputBufferSize = bufferSamples * audio->bufferFormat->nBlockAlign;

	// setup event handle to wait on
	audio->event = CreateEventW(NULL, FALSE, FALSE, NULL);
	hr = audio->client->SetEventHandle(audio->event);
	assert(SUCCEEDED(hr));

	// use at least 64KB or 1 second whichever is larger, and round upwards to pow2 for ringbuffer
	DWORD ringBufferSize = RoundUpPow2(max(64 * 1024, audio->bufferFormat->nAvgBytesPerSec));

	// Explanation of Magic Ring Buffer: https://fgiesen.wordpress.com/2012/07/21/the-magic-ring-buffer/
	// MSDN Example code: https://learn.microsoft.com/en-us/windows/win32/api/memoryapi/nf-memoryapi-virtualalloc2#examples
	
	// reserve virtual address placeholder for 2x size for magic ringbuffer
	char* placeholder1 = (char*)VirtualAlloc2(NULL, NULL, 2 * ringBufferSize, MEM_RESERVE | MEM_RESERVE_PLACEHOLDER, PAGE_NOACCESS, NULL, 0);
	char* placeholder2 = placeholder1 + ringBufferSize;
	assert(placeholder1);

	// split allocated address space in half
	BOOL ok = VirtualFree(placeholder1, ringBufferSize, MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER);
	assert(ok);

	// create page-file backed section for buffer
	HANDLE section = CreateFileMappingW(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0, ringBufferSize, NULL);
	assert(section);

	// map same section into both addresses
	void* view1 = MapViewOfFile3(section, NULL, placeholder1, 0, ringBufferSize, MEM_REPLACE_PLACEHOLDER, PAGE_READWRITE, NULL, 0);
	void* view2 = MapViewOfFile3(section, NULL, placeholder2, 0, ringBufferSize, MEM_REPLACE_PLACEHOLDER, PAGE_READWRITE, NULL, 0);
	assert(view1 && view2);

	// this is ok, actual memory will be freed only when it is unmapped
	VirtualFree(placeholder1, 0, MEM_RELEASE);
	VirtualFree(placeholder2, 0, MEM_RELEASE);
	CloseHandle(section);

	audio->buffer1 = (BYTE*)view1;
	audio->buffer2 = (BYTE*)view2;
	audio->ringBufferSize = ringBufferSize;
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

WasapiAudioLockContext Win32AudioLockBuffer(WasapiAudio* audio)
{
	UINT32 bytesPerSample = audio->bufferFormat->nBlockAlign;
	UINT32 ringBufferSize = audio->ringBufferSize;
	UINT32 outputBufferSize = audio->outputBufferSize;

	WA__Lock(audio);

	UINT32 readOffset = audio->rbReadOffset;
	UINT32 lockOffset = audio->rbLockOffset;
	UINT32 writeOffset = audio->rbWriteOffset;

	// how many bytes are in use by audio thread = [read, lock) range
	UINT32 numBytesInUse = lockOffset - readOffset;

	// make sure there are samples available for one wasapi buffer submission
	// so in case audio thread needs samples before UnlockBuffer is called, it can get some 
	if (numBytesInUse < outputBufferSize)
	{
		// Num bytes we've written to ringbuffer = [read, write) range
		// i.e. upper bound on what audio thread can submit to wasapi
		UINT32 numBytesWritten = writeOffset - readOffset;

		// if [read, lock) is smaller than outputBufferSize buffer, then try to increase
		// lock to [read, read+outputBufferSize) range (capped at the number of bytes written)
		numBytesInUse = min(outputBufferSize, numBytesWritten);
		audio->rbLockOffset = lockOffset = readOffset + numBytesInUse;
	}

	// how many bytes can be written to buffer
	UINT32 availableSize = ringBufferSize - numBytesInUse;

	// reset write marker to beginning of lock offset (can start writing there)
	audio->rbWriteOffset = lockOffset;

	UINT32 playCount = audio->numSamplesSubmittedSinceLastTick;
	audio->numSamplesSubmittedSinceLastTick = 0;

	WA__Unlock(audio);

	WasapiAudioLockContext context = {};
	context.numSamplesPlayedSinceLastTick = playCount;

	// buffer offset/size where to write
	// safe to write in [write, read) range, because reading happen in [read, lock) range (lock==write)
	context.outputSamples = (float*)(audio->buffer1 + (lockOffset & (ringBufferSize - 1)));
	
	// write at least 100msec of samples into buffer (or whatever space available, whichever is smaller)
	// this is max amount of time you expect code will take until the next iteration of loop
	// if code will take more time then you'll hear discontinuity as buffer will be filled with silence
	UINT32 numSamplesAvailable = availableSize / bytesPerSample;
	context.numSamplesToWrite = min(audio->bufferFormat->nSamplesPerSec/10, numSamplesAvailable);
	// alternatively you can write as much as "audio.sampleCount" to fully fill the buffer (~1 second)
	// then you can try to increase delay below to 900+ msec, it still should sound fine
	//numSamplesToWrite = audio.sampleCount;
	
	memset(context.outputSamples, 0, context.numSamplesToWrite * bytesPerSample);

	return context;
}

void Win32AudioUnlockBuffer(WasapiAudio* audio, size_t numSamplesWritten)
{
	UINT32 bytesPerSample = audio->bufferFormat->nBlockAlign;
	size_t numBytesWritten = numSamplesWritten * bytesPerSample;

	// advance write offset to allow reading new samples
	InterlockedAdd(&audio->rbWriteOffset, (LONG)numBytesWritten);
}
