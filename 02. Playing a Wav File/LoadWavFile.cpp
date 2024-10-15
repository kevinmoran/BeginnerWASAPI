#include "LoadWavFile.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmsystem.h>
#include <aviriff.h>
#include <assert.h>

// Extremely rudimentary and barebones Wav file loader
// For illustrative purposes only, no warranty is implied
// References: 
// https://www.mmsp.ece.mcgill.ca/Documents/AudioFormats/WAVE/WAVE.html
// Handmade Hero Day 138: Loading WAV Files

AudioClip parseWavFile(uint8_t* fileBytes, uint32_t fileSize)
{
    AudioClip result = {};

    RIFFLIST* header = (RIFFLIST*)fileBytes;
    if(header->fcc != FCC('RIFF') || header->fccListType != FCC('WAVE'))
    {
        assert(!"Invalid wav header");
        return result;
    }
    void* subChunks = fileBytes + sizeof(RIFFLIST);
    void* endOfFile = fileBytes + fileSize;
    for (
        RIFFCHUNK* chunk = (RIFFCHUNK*)(subChunks);
        chunk < endOfFile;
        chunk = RIFFNEXT(chunk)
        )
    {
        if(chunk->fcc == FCC('fmt ')) {
            WAVEFORMATEX* fmt = (WAVEFORMATEX*)(chunk+1);
            
            if(fmt->wFormatTag != WAVE_FORMAT_PCM)
            {
                assert(!"Unsupported format - PCM only");
                return result;
            }
            assert(chunk->cb == 16 || chunk->cb == 18); 
            assert(fmt->nBlockAlign == fmt->nChannels * fmt->wBitsPerSample/8);
            assert(fmt->nAvgBytesPerSec == fmt->nSamplesPerSec * fmt->nBlockAlign);

            result.numChannels = fmt->nChannels;
            result.sampleRate = fmt->nSamplesPerSec;
            result.numBitsPerSample = fmt->wBitsPerSample;
        }
        else if(chunk->fcc == FCC('data')) {
            result.numSamples = chunk->cb / sizeof(uint16_t);
            result.samples = ((uint8_t*)chunk + sizeof(RIFFCHUNK));
            assert((uint8_t*)result.samples + chunk->cb - 1 < endOfFile);
        }
    }
    return result;
}
