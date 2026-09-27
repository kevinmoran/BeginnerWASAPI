#include "Win32Wasapi.h"

#include <assert.h>

#include <audioclient.h>
#include <avrt.h>
#include <mmdeviceapi.h>

// Sample code for educational purposes
// Currently just calls assert() on each windows call, needs proper error handling

// TODO: Automatically switch to new device when default audio device changes (IMMNotificationClient)

#pragma comment(lib, "avrt")
#pragma comment(lib, "ole32")
#pragma comment(lib, "onecore")

// Forward declare internal functions
static DWORD CALLBACK _AudioThreadProc(LPVOID arg);

DWORD RoundUpPow2(DWORD value)
{
    unsigned long index;
    _BitScanReverse(&index, value - 1);
    assert(index < 31);
    return 1U << (index + 1);
}

void Win32AudioStart(Win32Audio* audio, size_t sampleRate, size_t channelCount, DWORD channelMask)
{
    *audio = {};

    HRESULT hr = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    assert(SUCCEEDED(hr));

    // Create enumerator to get audio device
    IMMDeviceEnumerator* enumerator;
    hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                          __uuidof(IMMDeviceEnumerator), (LPVOID*)(&enumerator));
    assert(SUCCEEDED(hr));

    // Get default playback device
    IMMDevice* device;
    hr = enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device);
    assert(SUCCEEDED(hr));
    enumerator->Release();

    // Create audio client for device
    hr = device->Activate(__uuidof(IAudioClient2), CLSCTX_ALL, NULL,
                          (LPVOID*)&audio->client);
    assert(SUCCEEDED(hr));
    device->Release();

    WAVEFORMATEXTENSIBLE formatEx = {};
    {
        formatEx.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
        formatEx.Format.nChannels = (WORD)channelCount;
        formatEx.Format.nSamplesPerSec = (WORD)sampleRate;
        formatEx.Format.nAvgBytesPerSec =
        (DWORD)(sampleRate * channelCount * sizeof(float));
        formatEx.Format.nBlockAlign = (WORD)(channelCount * sizeof(float));
        formatEx.Format.wBitsPerSample = (WORD)(8 * sizeof(float));
        formatEx.Format.cbSize = sizeof(formatEx) - sizeof(formatEx.Format);
        formatEx.Samples.wValidBitsPerSample = 8 * sizeof(float);
        formatEx.dwChannelMask = channelMask;
        formatEx.SubFormat = KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;
    }

    WAVEFORMATEX* wfx;
    if(sampleRate == 0 || channelCount == 0 || channelMask == 0)
    {
        // Use native mixing format
        hr = audio->client->GetMixFormat(&wfx);
        assert(SUCCEEDED(hr));
        audio->bufferFormat = wfx;
    }
    else
    {
        // Use requested format
        wfx = &formatEx.Format;
        audio->bufferFormat = (WAVEFORMATEX*)CoTaskMemAlloc(sizeof(formatEx));
        CopyMemory(audio->bufferFormat, &formatEx, sizeof(formatEx));
    }

    BOOL initSucceeded = FALSE;

    // Try to initialize client with newer functionality in Windows 10, no AUTOCONVERTPCM allowed
    IAudioClient3* client3;
    if(SUCCEEDED(audio->client->QueryInterface(__uuidof(IAudioClient3), (LPVOID*)&client3)))
    {
        // Minimum buffer size will typically be 480 samples (10msec @ 48khz)
        // but it can be 128 samples (2.66 msec @ 48khz) if driver is properly installed
        // See bullet-point instructions here: https://learn.microsoft.com/en-us/windows-hardware/drivers/audio/low-latency-audio#measurement-tools
        UINT32 defaultPeriodSamples, fundamentalPeriodSamples, minPeriodSamples, maxPeriodSamples;
        hr = client3->GetSharedModeEnginePeriod(wfx, &defaultPeriodSamples,
                                                &fundamentalPeriodSamples,
                                                &minPeriodSamples, &maxPeriodSamples);

        if(SUCCEEDED(hr))
        {
            const DWORD flags = AUDCLNT_STREAMFLAGS_EVENTCALLBACK;
            if(SUCCEEDED(client3->InitializeSharedAudioStream(flags, minPeriodSamples, wfx, NULL)))
            {
                initSucceeded = TRUE;
            }
        }
        client3->Release();
    }

    // If we couldn't initialize with IAudioClient3, fall back on older API
    if(!initSucceeded)
    {
        // Get device period for shared-mode streams, this will typically be 480 samples (10msec @ 48khz)
        REFERENCE_TIME devicePeriod;
        hr = audio->client->GetDevicePeriod(&devicePeriod, NULL);
        assert(SUCCEEDED(hr));

        const DWORD flags = AUDCLNT_STREAMFLAGS_EVENTCALLBACK |
        AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;
        hr = audio->client->Initialize(AUDCLNT_SHAREMODE_SHARED, flags,
                                       devicePeriod, 0, wfx, NULL);
        assert(SUCCEEDED(hr));
    }

    UINT32 bufferNumSamples;
    hr = audio->client->GetBufferSize(&bufferNumSamples);
    assert(SUCCEEDED(hr));
    audio->outputBufferNumBytes = bufferNumSamples * audio->bufferFormat->nBlockAlign;

    // Create event handle to wait on - WASAPI will signal it to request we submit samples
    audio->event = CreateEventW(NULL, FALSE, FALSE, NULL);
    hr = audio->client->SetEventHandle(audio->event);
    assert(SUCCEEDED(hr));

    // Use at least 64KB or 1 second (whichever is larger), and round upwards to pow2 for ringbuffer
    DWORD ringBufferNumBytes =
    RoundUpPow2(max(64 * 1024, audio->bufferFormat->nAvgBytesPerSec));

    // Explanation of Magic Ring Buffer: https://fgiesen.wordpress.com/2012/07/21/the-magic-ring-buffer/
    // MSDN Example code: https://learn.microsoft.com/en-us/windows/win32/api/memoryapi/nf-memoryapi-virtualalloc2#examples

    // Reserve virtual address placeholder for 2x size for magic ringbuffer
    char* placeholder1 = (char*)VirtualAlloc2(NULL, NULL, 2 * ringBufferNumBytes,
                                              MEM_RESERVE | MEM_RESERVE_PLACEHOLDER,
                                              PAGE_NOACCESS, NULL, 0);
    assert(placeholder1);
    char* placeholder2 = placeholder1 + ringBufferNumBytes;

    // Split allocated address space in half
    BOOL ok = VirtualFree(placeholder1, ringBufferNumBytes,
                          MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER);
    assert(ok);

    // Create page-file backed section for buffer
    HANDLE section = CreateFileMappingW(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE,
                                        0, ringBufferNumBytes, NULL);
    assert(section);

    // Map same section into both addresses
    void* view1 = MapViewOfFile3(section, NULL, placeholder1, 0, ringBufferNumBytes,
                                 MEM_REPLACE_PLACEHOLDER, PAGE_READWRITE, NULL, 0);
    void* view2 = MapViewOfFile3(section, NULL, placeholder2, 0, ringBufferNumBytes,
                                 MEM_REPLACE_PLACEHOLDER, PAGE_READWRITE, NULL, 0);
    assert(view1 && view2);

    // Free placeholders, actual memory will be freed only when it is unmapped
    VirtualFree(placeholder1, 0, MEM_RELEASE);
    VirtualFree(placeholder2, 0, MEM_RELEASE);
    CloseHandle(section);

    audio->buffer1 = (BYTE*)view1;
    audio->buffer2 = (BYTE*)view2;
    audio->ringBufferNumBytes = ringBufferNumBytes;
    InitializeSRWLock(&audio->lock);
    audio->thread = CreateThread(NULL, 0, &_AudioThreadProc, audio, 0, NULL);
}

void Win32AudioStop(Win32Audio* audio)
{
    // Notify thread to stop
    InterlockedExchange(&audio->stop, TRUE);
    SetEvent(audio->event);

    // Wait for thread to finish
    WaitForSingleObject(audio->thread, INFINITE);
    CloseHandle(audio->thread);
    CloseHandle(audio->event);

    // Release ringbuffer
    UnmapViewOfFileEx(audio->buffer1, 0);
    UnmapViewOfFileEx(audio->buffer2, 0);

    // Release audio client
    CoTaskMemFree(audio->bufferFormat);
    audio->client->Release();

    CoUninitialize();
}

Win32AudioWriteContext Win32AudioAcquireWriteContext(Win32Audio* audio)
{
    Win32AudioWriteContext context = {};

    UINT32 bytesPerSample = audio->bufferFormat->nBlockAlign;
    UINT32 ringBufferNumBytes = audio->ringBufferNumBytes;
    UINT32 outputBufferNumBytes = audio->outputBufferNumBytes;
    UINT32 sampleRate = audio->bufferFormat->nSamplesPerSec;

    AcquireSRWLockExclusive(&audio->lock);

    // How many bytes are in use by audio thread = [read, lock) range
    UINT32 numBytesInUse = audio->rbLockOffset - audio->rbReadOffset;

    // Make sure audio thread has locked enough samples to fill output buffer,
    // in case it gets woken before UnlockBuffer is called
    if(numBytesInUse < outputBufferNumBytes)
    {
        // Num bytes we've written to ringbuffer = [read, write) range
        // i.e. upper bound on what audio thread can submit to wasapi
        UINT32 numBytesWritten = audio->rbWriteOffset - audio->rbReadOffset;

        numBytesInUse = min(outputBufferNumBytes, numBytesWritten);
        audio->rbLockOffset = audio->rbReadOffset + numBytesInUse;
    }
    // Set write marker to end of locked region of ringbuffer
    audio->rbWriteOffset = audio->rbLockOffset;

    // How many bytes can be written to buffer
    UINT32 numBytesAvailable = ringBufferNumBytes - numBytesInUse;

    context.numSamplesPlayedSinceLastTick = audio->numSamplesSubmittedSinceLastTick;
    audio->numSamplesSubmittedSinceLastTick = 0;

    ReleaseSRWLockExclusive(&audio->lock);

    // (a % b) == (a & (b-1)) if b is a power of 2
    // UINT32 writeOffset = audio->rbWriteOffset % ringBufferNumBytes;
    UINT32 writeOffset = audio->rbWriteOffset & (ringBufferNumBytes - 1);

    UINT32 numSamplesAvailable = numBytesAvailable / bytesPerSample;

    // Set worstCaseTickTimeInSecs to the max amount of time you expect main
    // loop will take until the next tick (100ms here). If a tick exceeds this
    // time audio will stutter as audio thread will fill the gap with silence
    float worstCaseTickTimeInSecs = 0.1f;
    // This is the number of samples we will make sure are "speculatively" written
    // to the ringbuffer and available to the audio thread to submit at all times,
    // to avoid stutters if a tick runs long
    UINT32 numPaddingSamples = (UINT32)(sampleRate * worstCaseTickTimeInSecs);

    context.numSamplesToWrite = min(numPaddingSamples, numSamplesAvailable);

    // Return pointer to ringbuffer at write offset
    context.outputSamples = (float*)(audio->buffer1 + writeOffset);
    // Initialise output buffer to 0 for mixing
    memset(context.outputSamples, 0, context.numSamplesToWrite * bytesPerSample);

    return context;
}

void Win32AudioReleaseWriteContext(Win32Audio* audio, Win32AudioWriteContext context)
{
    UINT32 bytesPerSample = audio->bufferFormat->nBlockAlign;
    size_t numBytesWritten = context.numSamplesToWrite * bytesPerSample;

    // Advance write offset to allow audio thread to read new samples
    InterlockedAdd(&audio->rbWriteOffset, (LONG)numBytesWritten);
}

// Entry point for audio thread
static DWORD CALLBACK _AudioThreadProc(LPVOID arg)
{
    Win32Audio* audio = (Win32Audio*)arg;

    DWORD task = 0;
    HANDLE handle = AvSetMmThreadCharacteristicsW(L"Pro Audio", &task);
    assert(handle);

    IAudioClient* client = audio->client;

    IAudioRenderClient* renderClient;
    HRESULT hr = client->GetService(__uuidof(IAudioRenderClient), (LPVOID*)&renderClient);
    assert(SUCCEEDED(hr));

    UINT32 bufferNumSamples;
    hr = client->GetBufferSize(&bufferNumSamples);
    assert(SUCCEEDED(hr));

    hr = client->Start();
    assert(SUCCEEDED(hr));

    UINT32 bytesPerSample = audio->bufferFormat->nBlockAlign;
    UINT32 rbMask = audio->ringBufferNumBytes - 1;
    BYTE* ringBuffer = audio->buffer1;

    while(WaitForSingleObject(audio->event, INFINITE) == WAIT_OBJECT_0)
    {
        if(InterlockedExchange(&audio->stop, FALSE)) { break; }

        // How many submitted samples wasapi has left to use
        UINT32 numPaddingSamples;
        hr = client->GetCurrentPadding(&numPaddingSamples);
        assert(SUCCEEDED(hr));

        UINT32 maxNumSamplesToOutput = bufferNumSamples - numPaddingSamples;

        // Get output buffer from WASAPI
        BYTE* outputBuffer;
        hr = renderClient->GetBuffer(maxNumSamplesToOutput, &outputBuffer);
        assert(SUCCEEDED(hr));

        AcquireSRWLockExclusive(&audio->lock);

        // Num bytes available to read from ringbuffer
        UINT32 numBytesAvailable = audio->rbWriteOffset - audio->rbReadOffset;
        UINT32 numSamplesAvailable = numBytesAvailable / bytesPerSample;

        // Clamp to not exceed available space in wasapi buffer
        UINT32 numSamplesToSubmit = min(numSamplesAvailable, maxNumSamplesToOutput);

        UINT32 numBytesToRead = numSamplesToSubmit * bytesPerSample;

        // Lock the range of ringbuffer we will be reading - [read, lock)
        // so the main thread can't overwrite it
        audio->rbLockOffset = audio->rbReadOffset + numBytesToRead;

        DWORD flags = 0;
        // If we have no samples to submit, fill buffer with silence
        if(numSamplesToSubmit == 0)
        {
            numSamplesToSubmit = maxNumSamplesToOutput;
            flags = AUDCLNT_BUFFERFLAGS_SILENT;
        }

        audio->numSamplesSubmittedSinceLastTick += numSamplesToSubmit;

        // Can now unlock buffer for main thread, it won't write in
        // [read, lock) interval while we're copying to output buffer
        ReleaseSRWLockExclusive(&audio->lock);

        memcpy(outputBuffer, ringBuffer + (audio->rbReadOffset & rbMask), numBytesToRead);

        // Unlock bytes in [read, lock) interval of ringbuffer
        InterlockedAdd(&audio->rbReadOffset, numBytesToRead);

        // Submit output buffer to WASAPI
        hr = renderClient->ReleaseBuffer(numSamplesToSubmit, flags);
        assert(SUCCEEDED(hr));
    }

    // Stop playback
    hr = client->Stop();
    assert(SUCCEEDED(hr));
    renderClient->Release();

    AvRevertMmThreadCharacteristics(handle);
    return 0;
}
