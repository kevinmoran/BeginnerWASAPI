
// Simple demonstration of multithreaded audio using WASAPI
// Adapted from sample code by Mārtiņš Možeiko:
// https://gist.github.com/mmozeiko/5a5b168e61aff4c1eaec0381da62808f

#include <mfapi.h>
#include <stdio.h>

#include "Sound.h"
#include "Win32Wasapi.h"

int main()
{
    Win32Audio audio = {};
    size_t sampleRate = 48000;
    Win32AudioStart(&audio, sampleRate, 2, SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT);

    // Background "music" that will be looping
    Sound background = SoundLoad(L"C:/Windows/Media/Ring10.wav", sampleRate);
    background.isLooping = true;

    // One-shot sound effect
    Sound effect = SoundLoad(L"C:/Windows/Media/tada.wav", sampleRate);

    printf("Press SPACE for sound effect, D for small delay, or ESC to stop\n");

    HANDLE input = GetStdHandle(STD_INPUT_HANDLE);

    for(;;)
    {
        bool escPressed = false;
        bool spacePressed = false;
        bool delayPressed = false;

        while(WaitForSingleObject(input, 0) == WAIT_OBJECT_0)
        {
            INPUT_RECORD record;
            DWORD read;
            if(ReadConsoleInputW(input, &record, 1, &read) && read == 1 &&
               record.EventType == KEY_EVENT && record.Event.KeyEvent.bKeyDown)
            {
                switch(record.Event.KeyEvent.wVirtualKeyCode)
                {
                    case VK_ESCAPE: escPressed = true; break;
                    case VK_SPACE: spacePressed = true; break;
                    case 'D': delayPressed = true; break;
                }
            }
        }

        if(escPressed)
        {
            printf("stop!\n");
            break;
        }

        if(spacePressed)
        {
            printf("tada!\n");
            effect.pos = 0;
        }

        {
            Win32AudioWriteContext writeContext = Win32AudioAcquireWriteContext(&audio);
            size_t numSamplesToWrite = writeContext.numSamplesToWrite;

            // Advance sound playback positions
            size_t playCount = writeContext.numSamplesPlayedSinceLastTick;
            SoundUpdate(&background, playCount);
            SoundUpdate(&effect, playCount);

            // Mix sounds into output
            float* output = writeContext.outputSamples;
            SoundMix(output, numSamplesToWrite, 0.3f, &background);
            SoundMix(output, numSamplesToWrite, 0.8f, &effect);

            Win32AudioReleaseWriteContext(&audio, writeContext);
        }

        if(delayPressed)
        {
            // Simulate a big game code stutter (~5fps) with a delay
            printf("delay!\n");
            Sleep(5 * 17);
        }
        else
        {
            // ~60fps delay to simulate game code running
            Sleep(17);
        }

        printf(".");
        fflush(stdout);
    }

    Win32AudioStop(&audio);

    printf("Done!\n");

    return 0;
}
