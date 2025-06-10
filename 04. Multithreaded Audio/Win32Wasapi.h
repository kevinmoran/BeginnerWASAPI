#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

// "count" means sample count (for example, 1 sample = 2 floats for stereo)
// "offset" or "size" means byte count

struct tWAVEFORMATEX;
struct IAudioClient;

struct WasapiAudio
{
	// public part
	
	// describes sampleBuffer format
	tWAVEFORMATEX* bufferFormat;

	// use these values only between LockBuffer/UnlockBuffer calls
	void* sampleBuffer;  // ringbuffer for interleaved samples, no need to handle wrapping
	size_t sampleCount;  // how big is buffer in samples
	size_t numSamplesPlayedSinceLastTick;

	// private
	IAudioClient* client;
	HANDLE event;
	HANDLE thread;
	LONG stop;
	LONG lock;
	BYTE* buffer1;
	BYTE* buffer2;
	UINT32 outputBufferSize;	// output buffer size in bytes
	UINT32 ringBufferSize;		// ringbuffer size, always power of 2
	UINT32 numSamplesSubmittedSinceLastTick; // how many samples are used from buffer
	bool bufferFirstLock;       // true when Win32AudioLockBuffer is used at least once
	volatile LONG rbReadOffset; // offset to read from buffer
	volatile LONG rbLockOffset; // offset up to point in buffer that's in use
	volatile LONG rbWriteOffset; // offset up to point buffer is filled
};

// pass 0 for rate/count/mask to get default format of output device (use audio->bufferFormat)
// channelMask is bitmask of values from table here: https://learn.microsoft.com/en-us/windows/win32/api/mmreg/ns-mmreg-waveformatextensible#remarks
void Win32AudioStart(WasapiAudio* audio, size_t sampleRate, size_t numChannels, DWORD channelMask);

// stops the playback and releases resources
void Win32AudioStop(WasapiAudio* audio);

// once locked, then you're allowed to write samples into the ringbuffer
// use only sampleBuffer, sampleCount and numSamplesPlayedSinceLastTick members
void Win32AudioLockBuffer(WasapiAudio* audio);
void Win32AudioUnlockBuffer(WasapiAudio* audio, size_t writtenCount);
