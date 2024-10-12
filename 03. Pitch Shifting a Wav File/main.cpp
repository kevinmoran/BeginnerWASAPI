
// Simple example code to load a Wav file and play it with WASAPI
// with pitch-shifting, i.e. playback at different speeds.

#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>

#include <assert.h>
#include <stdint.h>

// Struct to get data from loaded WAV file.
// NB: This will only work for WAV files containing PCM (non-compressed) data
// otherwise the layout will be different.
#pragma warning(disable : 4200) // zero-sized array in struct/union
struct WavFile {
    // RIFF Chunk
    uint32_t riffId;
    uint32_t riffChunkSize;
    uint32_t waveId;

    // fmt Chunk
    uint32_t fmtId;
    uint32_t fmtChunkSize;
    uint16_t formatCode;
    uint16_t numChannels;
    uint32_t sampleRate;
    uint32_t byteRate;
    uint16_t blockAlign;
    uint16_t bitsPerSample;
    // These are not present for PCM Wav Files
    // uint16_t cbSize;
    // uint16_t wValidBitsPerSample;
    // uint32_t dwChannelMask;
    // char subFormatGUID[16];

    // data Chunk
    uint32_t dataId;
    uint32_t dataChunkSize;
    uint16_t samples[]; // actual samples start here
};
#pragma warning(default : 4200)

bool win32LoadEntireFile(const char* filename, void** data, uint32_t* numBytesRead)
{    
    HANDLE file = CreateFileA(filename, GENERIC_READ, 0, 0, OPEN_EXISTING, 0, 0);  
    if((file == INVALID_HANDLE_VALUE)) return false;
    
    DWORD fileSize = GetFileSize(file, 0);
    if(!fileSize) return false;
    
    *data = HeapAlloc(GetProcessHeap(), 0, fileSize+1);
    if(!*data) return false;

    if(!ReadFile(file, *data, fileSize, (LPDWORD)numBytesRead, 0))
        return false;
    
    CloseHandle(file);
    ((uint8_t*)*data)[fileSize] = 0;
    
    return true;
}

void Win32FreeFileData(void *data)
{
    HeapFree(GetProcessHeap(), 0, data);
}

inline float lerp(float a, float b, float t){
    return a + (b-a)*t;
}

int main()
{
    const char* wavFilename = "Flowing-Water.wav";
    void* fileBytes;
    uint32_t fileSize;
    bool loadedWavSuccessfully = win32LoadEntireFile(wavFilename, &fileBytes, &fileSize);
    assert(loadedWavSuccessfully);

    WavFile* wav = (WavFile*)fileBytes;
    // Check the Chunk IDs to make sure we loaded the file correctly
    assert(wav->riffId == 1179011410);
    assert(wav->waveId == 1163280727);
    assert(wav->fmtId == 544501094);
    assert(wav->dataId == 1635017060);
    // Check data is in format we expect
    assert(wav->formatCode == 1); // Only support PCM data
    assert(wav->numChannels == 1 || wav->numChannels == 2); // Only support 1- or 2-channel data
    assert(wav->fmtChunkSize == 16); // This should be true for PCM data
    assert(wav->bitsPerSample == 16); // Only support 16-bit samples
    // This is how these fields are defined, no harm to assert that they're what we expect
    // assert(wav->blockAlign == wav->numChannels * wav->bitsPerSample/8);
    assert(wav->blockAlign == wav->numChannels * 2); // bitsPerSample is always 16
    assert(wav->byteRate == wav->sampleRate * wav->blockAlign);

    uint32_t numWavSamples = wav->dataChunkSize / sizeof(uint16_t);
    uint32_t numWavFrames = numWavSamples / wav->numChannels;

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
    const float wavDurationSecs = (float)numWavFrames/wav->sampleRate;
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
            float currFrameIndex = wavPlaybackTime * wav->sampleRate;
            int prevFrameIndex = (int)currFrameIndex;
            int nextFrameIndex = (prevFrameIndex + 1) % numWavFrames;

            int prevLeftSampleIndex = wav->numChannels * prevFrameIndex;
            int nextLeftSampleIndex = wav->numChannels * nextFrameIndex;
            int prevRightSampleIndex = prevLeftSampleIndex + wav->numChannels - 1;
            int nextRightSampleIndex = nextLeftSampleIndex + wav->numChannels - 1;

            float prevLeftSample = (float)(wav->samples[prevLeftSampleIndex]);
            float nextLeftSample = (float)(wav->samples[nextLeftSampleIndex]);
            float prevRightSample = (float)(wav->samples[prevRightSampleIndex]);
            float nextRightSample = (float)(wav->samples[nextRightSampleIndex]);

            float lerpT = (currFrameIndex - prevFrameIndex) / wav->sampleRate;
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
