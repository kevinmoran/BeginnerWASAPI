#include <stdint.h>

struct AudioClip {
    uint32_t numChannels;
    uint32_t numBitsPerSample;
    uint32_t sampleRate;
    uint32_t numSamples;
    void* samples;
};

AudioClip parseWavFile(uint8_t* fileBytes, uint32_t fileSize);
