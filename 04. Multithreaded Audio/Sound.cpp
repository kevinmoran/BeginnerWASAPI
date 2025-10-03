#include "Sound.h"

#include <assert.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>

#pragma comment(lib, "mfplat")
#pragma comment(lib, "mfreadwrite")

Sound SoundLoad(const wchar_t* path, size_t sampleRate)
{
    Sound sound = {};
    HRESULT hr = MFStartup(MF_VERSION, MFSTARTUP_LITE);
    assert(SUCCEEDED(hr));

    IMFSourceReader* reader;
    hr = MFCreateSourceReaderFromURL(path, NULL, &reader);
    assert(SUCCEEDED(hr));

    // read only first audio stream
    hr = reader->SetStreamSelection((DWORD)MF_SOURCE_READER_ALL_STREAMS, FALSE);
    assert(SUCCEEDED(hr));
    hr = reader->SetStreamSelection((DWORD)MF_SOURCE_READER_FIRST_AUDIO_STREAM, TRUE);
    assert(SUCCEEDED(hr));

    const size_t kChannelCount = 1;
    WAVEFORMATEXTENSIBLE format = {};
    format.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    format.Format.nChannels = (WORD)kChannelCount;
    format.Format.nSamplesPerSec = (WORD)sampleRate;
    format.Format.nAvgBytesPerSec = (DWORD)(sampleRate * kChannelCount * sizeof(short));
    format.Format.nBlockAlign = (WORD)(kChannelCount * sizeof(short));
    format.Format.wBitsPerSample = (WORD)(8 * sizeof(short));
    format.Format.cbSize = sizeof(format) - sizeof(format.Format);
    format.Samples.wValidBitsPerSample = 8 * sizeof(short);
    format.dwChannelMask = SPEAKER_FRONT_CENTER;
    format.SubFormat = KSDATAFORMAT_SUBTYPE_PCM;

    // Media Foundation in Windows 8+ allows reader to convert output to different format than native
    IMFMediaType* type;
    hr = MFCreateMediaType(&type);
    assert(SUCCEEDED(hr));
    hr = MFInitMediaTypeFromWaveFormatEx(type, &format.Format, sizeof(format));
    assert(SUCCEEDED(hr));
    hr = reader->SetCurrentMediaType((DWORD)MF_SOURCE_READER_FIRST_AUDIO_STREAM, NULL, type);
    assert(SUCCEEDED(hr));
    type->Release();

    size_t used = 0;
    size_t capacity = 0;

    for(;;)
    {
        IMFSample* sample;
        DWORD flags = 0;
        hr = reader->ReadSample((DWORD)MF_SOURCE_READER_FIRST_AUDIO_STREAM, 0,
                                NULL, &flags, NULL, &sample);
        if(FAILED(hr)) { break; }

        if(flags & MF_SOURCE_READERF_ENDOFSTREAM) { break; }
        assert(flags == 0);

        IMFMediaBuffer* buffer;
        hr = sample->ConvertToContiguousBuffer(&buffer);
        assert(SUCCEEDED(hr));

        BYTE* data;
        DWORD size;
        hr = buffer->Lock(&data, NULL, &size);
        assert(SUCCEEDED(hr));
        {
            size_t avail = capacity - used;
            if(avail < size)
            {
                sound.samples = (short*)realloc(sound.samples, capacity += 64 * 1024);
            }
            memcpy((char*)sound.samples + used, data, size);
            used += size;
        }
        hr = buffer->Unlock();
        assert(SUCCEEDED(hr));

        buffer->Release();
        sample->Release();
    }

    reader->Release();

    hr = MFShutdown();
    assert(SUCCEEDED(hr));

    sound.pos = sound.numSamples = used / format.Format.nBlockAlign;
    return sound;
}

void SoundUpdate(Sound* sound, size_t samples)
{
    sound->pos += samples;
    if(sound->isLooping) { sound->pos %= sound->numSamples; }
    else { sound->pos = min(sound->pos, sound->numSamples); }
}

void SoundMix(float* outSamples, size_t outSampleCount, float volume, const Sound* sound)
{
    const short* inSamples = sound->samples;
    size_t inPos = sound->pos;
    size_t inCount = sound->numSamples;
    bool inLoop = sound->isLooping;

    for(size_t i = 0; i < outSampleCount; i++)
    {
        if(inLoop)
        {
            if(inPos == inCount)
            {
                // reset looping sound back to start
                inPos = 0;
            }
        }
        else
        {
            if(inPos >= inCount)
            {
                // non-looping sounds stops playback when done
                break;
            }
        }

        float sample = inSamples[inPos++] * (1.f / 32768.f);
        outSamples[0] += volume * sample;
        outSamples[1] += volume * sample;
        outSamples += 2;
    }
}
