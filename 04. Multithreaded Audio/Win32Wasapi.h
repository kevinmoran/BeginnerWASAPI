#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

struct tWAVEFORMATEX;
struct IAudioClient;

struct WasapiAudio
{
	IAudioClient* client;
	HANDLE event;
	HANDLE thread;
	LONG stop;
	
	BYTE* buffer1;
	BYTE* buffer2;
	tWAVEFORMATEX* bufferFormat;
	UINT32 outputBufferSize; // in bytes
	UINT32 ringBufferSize; // in bytes, always power of 2
	
	LONG lock;
	UINT32 numSamplesSubmittedSinceLastTick;
	volatile LONG rbReadOffset; // offset to read from buffer
	volatile LONG rbLockOffset; // offset up to point in buffer that's in use
	volatile LONG rbWriteOffset; // offset up to point buffer is filled
};

struct WasapiAudioLockContext
{
	float* outputSamples;
	size_t numSamplesToWrite;
	size_t numSamplesPlayedSinceLastTick;
};

// pass 0 for rate/count/mask to get default format of output device (use audio->bufferFormat)
// channelMask is bitmask of values from table here: https://learn.microsoft.com/en-us/windows/win32/api/mmreg/ns-mmreg-waveformatextensible#remarks
void Win32AudioStart(WasapiAudio* audio, size_t sampleRate, size_t numChannels, DWORD channelMask);

// stops the playback and releases resources
void Win32AudioStop(WasapiAudio* audio);

// once locked, then you're allowed to write samples into the ringbuffer
// use only sampleBuffer, sampleCount and numSamplesPlayedSinceLastTick members
WasapiAudioLockContext Win32AudioLockBuffer(WasapiAudio* audio);
void Win32AudioUnlockBuffer(WasapiAudio* audio, size_t writtenCount);
