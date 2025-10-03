#pragma once

struct Sound
{
    short* samples;
    size_t numSamples;
    size_t pos;
    bool isLooping;
};

// loads any supported sound file, and resamples to mono 16-bit audio with specified sample rate
Sound SoundLoad(const wchar_t* path, size_t sampleRate);
void SoundUpdate(Sound* sound, size_t samples);
void SoundMix(float* outSamples, size_t outSampleCount, float volume, const Sound* sound);
