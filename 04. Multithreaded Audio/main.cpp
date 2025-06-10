
// Simple demonstration of multithreaded audio using WASAPI
// Adapted from sample code by Mārtiņš Možeiko:
// https://gist.github.com/mmozeiko/5a5b168e61aff4c1eaec0381da62808f

#include <stdio.h>
#include <mfapi.h>

#include "Sound.h"
#include "Win32Wasapi.h"

int main()
{
	WasapiAudio audio = {};
	Win32AudioStart(&audio, 48000, 2, SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT);
	size_t sampleRate = audio.bufferFormat->nSamplesPerSec;
	size_t bytesPerSample = audio.bufferFormat->nBlockAlign;

	// background "music" that will be looping
	Sound background = SoundLoad(L"C:/Windows/Media/Ring10.wav", sampleRate);
	background.isLooping = true;

	// simple sound effect, won't be looping
	Sound effect = SoundLoad(L"C:/Windows/Media/tada.wav", sampleRate);

	printf("Press SPACE for sound effect, D for small delay, or ESC to stop\n");

	HANDLE input = GetStdHandle(STD_INPUT_HANDLE);

	for (;;)
	{
		bool escPressed = false;
		bool spacePressed = false;
		bool delayPressed = false;

		while (WaitForSingleObject(input, 0) == WAIT_OBJECT_0)
		{
			INPUT_RECORD record;
			DWORD read;
			if (ReadConsoleInputW(input, &record, 1, &read)
				&& read == 1
				&& record.EventType == KEY_EVENT
				&& record.Event.KeyEvent.bKeyDown)
			{
				switch (record.Event.KeyEvent.wVirtualKeyCode)
				{
				case VK_ESCAPE: escPressed = true; break;
				case VK_SPACE: spacePressed = true; break;
				case 'D': delayPressed = true; break;
				}
			}
		}

		if (escPressed)
		{
			printf("stop!\n");
			break;
		}

		if (spacePressed)
		{
			printf("tada!\n");
			effect.pos = 0;
		}

		{
			Win32AudioLockBuffer(&audio);

			// write at least 100msec of samples into buffer (or whatever space available, whichever is smaller)
			// this is max amount of time you expect code will take until the next iteration of loop
			// if code will take more time then you'll hear discontinuity as buffer will be filled with silence
			size_t writeCount = min(sampleRate/10, audio.sampleCount);

			// alternatively you can write as much as "audio.sampleCount" to fully fill the buffer (~1 second)
			// then you can try to increase delay below to 900+ msec, it still should sound fine
			//writeCount = audio.sampleCount;

			// advance sound playback positions
			size_t playCount = audio.playCount;
			SoundUpdate(&background, playCount);
			SoundUpdate(&effect, playCount);

			printf("Writecount: %u, Playcount: %u\n", writeCount, playCount);

			// initialize output with 0.0f
			float* output = (float*)audio.sampleBuffer;
			memset(output, 0, writeCount * bytesPerSample);

			// mix sounds into output
			SoundMix(output, writeCount, 0.3f, &background);
			SoundMix(output, writeCount, 0.8f, &effect);

			Win32AudioUnlockBuffer(&audio, writeCount);
		}

		if (delayPressed)
		{
			printf("delay!\n");
			Sleep(5 * 17); // large delay for ~5 frames = ~68 msec
			//Sleep(900);
		}
		else
		{
			// just a small delay, pretend this is your normal rendering code
			Sleep(17); // "60" fps
		}

		printf(".");
		fflush(stdout);
	}

	Win32AudioStop(&audio);

	printf("Done!\n");

	return 0;
}