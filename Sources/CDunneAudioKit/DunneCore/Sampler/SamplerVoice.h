// Copyright AudioKit. All Rights Reserved.

#pragma once
#include <math.h>

#include "SampleBuffer.h"
#include "SampleOscillator.h"
#include "ADSREnvelope.h"
#include "AHDSHREnvelope.h"
#include "FunctionTable.h"
#include "ResonantLowPassFilter.h"
#include "LinearRamper.h"

// process samples in "chunks" this size
#define CORESAMPLER_CHUNKSIZE 16

namespace DunneCore
{
    struct SamplerVoice
    {
        float samplingRate;
        SampleOscillator oscillator;
        SampleBuffer *sampleBuffer;
        ResonantLowPassFilter leftFilter, rightFilter;
        AHDSHREnvelope ampEnvelope;
        ADSREnvelope filterEnvelope, pitchEnvelope;
        FunctionTableOscillator vibratoLFO;
        bool restartVoiceLFO;
        float *glideSecPerOctave;
        int noteNumber;
        float noteFrequency;
        float glideSemitones;
        float pitchEnvelopeSemitones;
        float voiceLFOSemitones;
        float noteVolume;
        float gain;
        float pan;
        float tempNoteVolume;
        SampleBuffer *newSampleBuffer;
        float tempGain;
        LinearRamper volumeRamper;
        bool isFilterEnabled;

        SamplerVoice() : noteNumber(-1), gain(0.0f), pan(0.0f) {}

                void init(double sampleRate);
                void setGain(float gainDB);
                void setPan(float panValue); // Set the pan value
                void updateAmpAdsrParameters() { ampEnvelope.updateParams(); }
                void updateFilterAdsrParameters() { filterEnvelope.updateParams(); }
                void updatePitchAdsrParameters() { pitchEnvelope.updateParams(); }

                void start(unsigned noteNumber,
                           float sampleRate,
                           float frequency,
                           float volume,
                           SampleBuffer *sampleBuffer);
                void restartNewNote(unsigned noteNumber, float sampleRate, float frequency, float volume, SampleBuffer *buffer);
                void restartNewNoteLegato(unsigned noteNumber, float sampleRate, float frequency);
                void restartSameNote(float volume, SampleBuffer *sampleBuffer);
                void release(bool loopThruRelease);
                void stop();
                
                bool prepToGetSamples(int sampleCount,
                                      float masterVolume,
                                      float pitchOffset,
                                      float cutoffMultiple,
                                      float keyTracking,
                                      float cutoffEnvelopeStrength,
                                      float cutoffEnvelopeVelocityScaling,
                                      float resLinear,
                                      float pitchADSRSemitones,
                                      float voiceLFOFrequencyHz,
                                      float voiceLFODepthSemitones);

                bool getSamples(int sampleCount, float *leftOutput, float *rightOutput);

            private:
                bool hasStartedVoiceLFO;
                void restartVoiceLFOIfNeeded();
            };
        }
