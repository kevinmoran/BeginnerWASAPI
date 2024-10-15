
// Simple example code to load a Wav file and play it with WASAPI
// This example implements resampling of the wav data to support
// different sample rates and to allow playback at different speeds

#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>

#include <assert.h>
#include <stdint.h>

#include "LoadWavFile.h"
#include "Win32LoadEntireFile.h"

inline float lerp(float a, float b, float t){
    return a + (b-a)*t;
}

int main()
{
    const char* wavFilename = "Testing48kHz.wav";
    void* fileBytes;
    uint32_t fileSize;
    bool result = win32LoadEntireFile(wavFilename, &fileBytes, &fileSize);
    assert(result);

    AudioClip clip = parseWavFile((uint8_t*)fileBytes, fileSize);
    assert(clip.numChannels == 2 || clip.numChannels == 1);
    assert(clip.numBitsPerSample == 16);

    HRESULT hr = CoInitializeEx(nullptr, COINIT_SPEED_OVER_MEMORY);
    assert(hr == S_OK);

    IMMDeviceEnumerator* deviceEnumerator;
    hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator), (LPVOID*)(&deviceEnumerator));
    assert(hr == S_OK);

    IMMDevice* audioDevice;
    hr = deviceEnumerator->GetDefaultAudioEndpoint(eRender, eConsole, &audioDevice);
    assert(hr == S_OK);

    deviceEnumerator->Release();

    IAudioClient2* audioClient;
    hr = audioDevice->Activate(__uuidof(IAudioClient2), CLSCTX_ALL, nullptr, (LPVOID*)(&audioClient));
    assert(hr == S_OK);

    audioDevice->Release();

    const int32_t OUTPUT_SAMPLE_RATE = 44100;
    WAVEFORMATEX mixFormat = {};
    mixFormat.wFormatTag = WAVE_FORMAT_PCM;
    mixFormat.nChannels = 2;
    mixFormat.nSamplesPerSec = OUTPUT_SAMPLE_RATE;
    mixFormat.wBitsPerSample = 16;
    mixFormat.nBlockAlign = (mixFormat.nChannels * mixFormat.wBitsPerSample) / 8;
    mixFormat.nAvgBytesPerSec = mixFormat.nSamplesPerSec * mixFormat.nBlockAlign;

    const float BUFFER_SIZE_IN_SECONDS = 2.0f;
    const int64_t REFTIMES_PER_SEC = 10000000; // hundred nanoseconds
    REFERENCE_TIME requestedSoundBufferDuration = (REFERENCE_TIME)(REFTIMES_PER_SEC * BUFFER_SIZE_IN_SECONDS);
    DWORD initStreamFlags = ( AUDCLNT_STREAMFLAGS_RATEADJUST 
                            | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM
                            | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY );
    hr = audioClient->Initialize(AUDCLNT_SHAREMODE_SHARED, 
                                 initStreamFlags, 
                                 requestedSoundBufferDuration, 
                                 0, &mixFormat, nullptr);
    assert(hr == S_OK);

    IAudioRenderClient* audioRenderClient;
    hr = audioClient->GetService(__uuidof(IAudioRenderClient), (LPVOID*)(&audioRenderClient));
    assert(hr == S_OK);

    UINT32 bufferSizeInFrames;
    hr = audioClient->GetBufferSize(&bufferSizeInFrames);
    assert(hr == S_OK);

    // IAudioClock* audioClock;
    // hr = audioClient->GetService(__uuidof(IAudioClock), (LPVOID*)(&audioClock));
    // assert(hr == S_OK);
    // UINT64 audioPlaybackFreq;
    // hr = audioClock->GetFrequency(&audioPlaybackFreq);
    // assert(hr == S_OK);

    hr = audioClient->Start();
    assert(hr == S_OK);

    float wavPlaybackTime = 0.0f;
    float wavPlaybackSpeed = 1.0f; // change this to speed up/slow down playback!
    const float wavDurationSecs = (float)clip.numSamples/(clip.sampleRate * clip.numChannels);
    bool playLooping = true;

    bool isRunning = true;
    while (isRunning)
    {
        // Padding is how much valid data is queued up in the sound buffer
        // if there's enough padding then we could skip writing more data
        UINT32 bufferPadding;
        hr = audioClient->GetCurrentPadding(&bufferPadding);
        assert(hr == S_OK);

        // How much padding we want our sound buffer to have after writing to it.
        // Needs to be enough so that the playback doesn't reach garbage data
        // but we get less latency the lower it is (i.e. how long does it take
        // between pressing jump and hearing the sound effect)
        // Try setting this to e.g. 1/250.f to hear what happens when
        // we're not writing enough data to stay ahead of playback!
        const float TARGET_BUFFER_PADDING_IN_SECONDS = 1/60.f;
        UINT32 targetBufferPadding = UINT32(bufferSizeInFrames * TARGET_BUFFER_PADDING_IN_SECONDS);
        UINT32 numFramesToWrite = targetBufferPadding - bufferPadding;

        int16_t* buffer;
        hr = audioRenderClient->GetBuffer(numFramesToWrite, (BYTE**)(&buffer));
        assert(hr == S_OK);

        for (UINT32 frameIndex = 0; frameIndex < numFramesToWrite; ++frameIndex)
        {
            float currFrameIndex = wavPlaybackTime * clip.sampleRate;
            int prevFrameIndex = (int)currFrameIndex;
            int nextFrameIndex = (prevFrameIndex + 1) % (clip.numSamples * clip.numChannels);

            int prevLeftSampleIndex = clip.numChannels * prevFrameIndex;
            int nextLeftSampleIndex = clip.numChannels * nextFrameIndex;
            int prevRightSampleIndex = prevLeftSampleIndex + clip.numChannels - 1;
            int nextRightSampleIndex = nextLeftSampleIndex + clip.numChannels - 1;

            uint16_t* samples = (uint16_t*)clip.samples;
            float prevLeftSample = (float)(samples[prevLeftSampleIndex]);
            float nextLeftSample = (float)(samples[nextLeftSampleIndex]);
            float prevRightSample = (float)(samples[prevRightSampleIndex]);
            float nextRightSample = (float)(samples[nextRightSampleIndex]);

            float lerpT = (currFrameIndex - prevFrameIndex) / clip.sampleRate;
            uint16_t leftSample = (uint16_t)lerp(prevLeftSample, nextLeftSample, lerpT);
            uint16_t rightSample = (uint16_t)lerp(prevRightSample, nextRightSample, lerpT);
            *buffer++ = leftSample;
            *buffer++ = rightSample;

            wavPlaybackTime += wavPlaybackSpeed / OUTPUT_SAMPLE_RATE;
            
            if(wavPlaybackTime >= wavDurationSecs){
                // Reached end of sound. Can choose to loop or stop
                wavPlaybackTime -= wavDurationSecs;
                if(!playLooping) {
                    isRunning = false;
                    break;
                }
            }
        }
        hr = audioRenderClient->ReleaseBuffer(numFramesToWrite, 0);
        assert(hr == S_OK);

        // Get playback cursor position
        // This is good for visualising playback and seeing the reading/writing in action!
        // UINT64 audioPlaybackPos;
        // hr = audioClock->GetPosition(&audioPlaybackPos, 0);
        // assert(hr == S_OK);
        // float audioPlaybackPosInSeconds = (float)audioPlaybackPos/audioPlaybackFreq;
        // int64_t audioPlaybackPosInSamples = (int64_t)(audioPlaybackPosInSeconds*OUTPUT_SAMPLE_RATE);
    }

    Sleep(1000);

    audioClient->Stop();
    audioClient->Release();
    audioRenderClient->Release();
    // audioClock->Release();

    Win32FreeFileData(fileBytes);

    return 0;
}
