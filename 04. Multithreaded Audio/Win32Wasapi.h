#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

struct tWAVEFORMATEX;
struct IAudioClient;

struct Win32Audio
{
    IAudioClient* client;
    HANDLE event;
    HANDLE thread;
    LONG stop;

    BYTE* buffer1;
    BYTE* buffer2;
    tWAVEFORMATEX* bufferFormat;
    UINT32 outputBufferNumBytes;
    UINT32 ringBufferNumBytes; // always power of 2

    SRWLOCK lock;
    UINT32 numSamplesSubmittedSinceLastTick;
    volatile LONG rbReadOffset; // offset for audio thread to read from buffer
    volatile LONG rbLockOffset; // offset to end of region audio thread is reading
    volatile LONG rbWriteOffset; // offset to point main loop has written to
};

struct Win32AudioWriteContext
{
    float* outputSamples;
    size_t numSamplesToWrite;
    size_t numSamplesPlayedSinceLastTick;
};

// Pass 0 for rate/count/mask to get default format of output device (use audio->bufferFormat)
// channelMask is bitmask of values from table here: https://learn.microsoft.com/en-us/windows/win32/api/mmreg/ns-mmreg-waveformatextensible#remarks
void Win32AudioStart(Win32Audio* audio, size_t sampleRate, size_t numChannels, DWORD channelMask);

// Stops playback and releases resources
void Win32AudioStop(Win32Audio* audio);

// Lock a region of audio buffer for writing, returned in context struct
Win32AudioWriteContext Win32AudioAcquireWriteContext(Win32Audio* audio);
// Releases previously locked region of buffer to audio thread for reading
void Win32AudioReleaseWriteContext(Win32Audio* audio, Win32AudioWriteContext context);
