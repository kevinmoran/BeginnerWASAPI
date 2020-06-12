
// Simple example code to load a Wav file and play it with WASAPI
// This is NOT complete Wav loading code. It is a barebones example 
// that makes a lot of assumptions, see the assert() calls for details
//
// References: 
// http://www-mmsp.ece.mcgill.ca/Documents/AudioFormats/WAVE/WAVE.html
// Handmade Hero Day 138: Loading WAV Files

#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>

#include <assert.h>
#define _USE_MATH_DEFINES
#include <math.h> // for sin()
#include <stdint.h>

// Struct to get data from loaded WAV file.
// NB: This will only work for WAV files containing PCM (non-compressed) data
// otherwise the layout will be different.
#pragma warning(disable : 4200)
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

int main()
{
    const char* wavFilename = "Flowing-Water.wav";
    // const char* wavFilename = "Engine-Noise.wav";
    void* fileBytes;
    uint32_t fileSize;
    bool result = win32LoadEntireFile(wavFilename, &fileBytes, &fileSize);
    assert(result);

    WavFile* wav = (WavFile*)fileBytes;
    // Check the Chunk IDs to make sure we loaded the file correctly
    assert(wav->riffId == 1179011410);
    assert(wav->waveId == 1163280727);
    assert(wav->fmtId == 544501094);
    assert(wav->dataId == 1635017060);
    // Check data is in format we expect
    assert(wav->formatCode == 1); // Only support PCM data
    assert(wav->numChannels == 2); // Only support 2-channel data
    assert(wav->fmtChunkSize == 16); // This should be true for PCM data
    assert(wav->sampleRate == 44100); // Only support 44100Hz data
    assert(wav->bitsPerSample == 16); // Only support 16-bit samples
    // This is how these fields are defined, no harm to assert that they're what we expect
    assert(wav->blockAlign == wav->numChannels * wav->bitsPerSample/8);
    assert(wav->byteRate == wav->sampleRate * wav->blockAlign);

    uint32_t numWavSamples = wav->dataChunkSize / (wav->numChannels * sizeof(uint16_t));
    uint16_t* wavSamples = wav->samples;

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
    
    // WAVEFORMATEX* defaultMixFormat = NULL;
    // hr = audioClient->GetMixFormat(&defaultMixFormat);
    // assert(hr == S_OK);

    WAVEFORMATEX mixFormat = {};
    mixFormat.wFormatTag = WAVE_FORMAT_PCM;
    mixFormat.nChannels = 2;
    mixFormat.nSamplesPerSec = 44100;//defaultMixFormat->nSamplesPerSec;
    mixFormat.wBitsPerSample = 16;
    mixFormat.nBlockAlign = (mixFormat.nChannels * mixFormat.wBitsPerSample) / 8;
    mixFormat.nAvgBytesPerSec = mixFormat.nSamplesPerSec * mixFormat.nBlockAlign;

    const int64_t REFTIMES_PER_SEC = 10000000; // hundred nanoseconds
    REFERENCE_TIME requestedSoundBufferDuration = REFTIMES_PER_SEC * 2;
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

    hr = audioClient->Start();
    assert(hr == S_OK);

    double playbackTime = 0.0;
    const float TONE_HZ = 440;
    const int16_t TONE_VOLUME = 3000;
    uint32_t wavPlaybackSample = 0;
    while (true)
    {
        // Padding is how much valid data is queued up in the sound buffer
        // if there's enough padding then we could skip writing more data
        UINT32 bufferPadding;
        hr = audioClient->GetCurrentPadding(&bufferPadding);
        assert(hr == S_OK);

        // How much of our sound buffer we want to fill on each update.
        // Needs to be enough so that the playback doesn't reach garbage data
        // but we get less latency the lower it is (e.g. how long does it take
        // between pressing jump and hearing the sound effect)
        // Try setting this to e.g. bufferSizeInFrames / 250 to hear what happens when
        // we're not writing enough data to stay ahead of playback!
        UINT32 soundBufferLatency = bufferSizeInFrames / 50;
        UINT32 numFramesToWrite = soundBufferLatency - bufferPadding;

        int16_t* buffer;
        hr = audioRenderClient->GetBuffer(numFramesToWrite, (BYTE**)(&buffer));
        assert(hr == S_OK);

        for (UINT32 frameIndex = 0; frameIndex < numFramesToWrite; ++frameIndex)
        {
            uint32_t leftSampleIndex = wav->numChannels*wavPlaybackSample;
            uint32_t rightSampleIndex = leftSampleIndex + wav->numChannels - 1;
            uint16_t leftSample = wav->samples[leftSampleIndex];
            uint16_t rightSample = wav->samples[rightSampleIndex];
            ++wavPlaybackSample;
            *buffer++ = leftSample;
            *buffer++ = rightSample;

            if(wavPlaybackSample >= numWavSamples)
                wavPlaybackSample -= numWavSamples;
        }
        hr = audioRenderClient->ReleaseBuffer(numFramesToWrite, 0);
        assert(hr == S_OK);

        // Get playback cursor position
        // This is good for visualising playback and seeing the reading/writing in action!
        IAudioClock* audioClock;
        audioClient->GetService(__uuidof(IAudioClock), (LPVOID*)(&audioClock));
        UINT64 audioPlaybackFreq;
        UINT64 audioPlaybackPos;
        audioClock->GetFrequency(&audioPlaybackFreq);
        audioClock->GetPosition(&audioPlaybackPos, 0);
        audioClock->Release();
        UINT64 audioPlaybackPosInSeconds = audioPlaybackPos/audioPlaybackFreq;
        UINT64 audioPlaybackPosInSamples = audioPlaybackPosInSeconds*mixFormat.nSamplesPerSec;
    }

    audioClient->Stop();
    audioClient->Release();
    audioRenderClient->Release();

    Win32FreeFileData(fileBytes);

    return 0;
}
