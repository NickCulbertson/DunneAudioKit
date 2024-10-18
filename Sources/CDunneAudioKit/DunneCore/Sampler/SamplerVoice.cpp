// Copyright AudioKit. All Rights Reserved.

#include "SamplerVoice.h"
#include <stdio.h>

#define MIDDLE_C_HZ 262.626f

namespace DunneCore
{
    void SamplerVoice::init(double sampleRate)
    {
        samplingRate = float(sampleRate);
        leftFilter.init(sampleRate);
        rightFilter.init(sampleRate);
        ampEnvelope.init();
        filterEnvelope.init();
        pitchEnvelope.init();
        vibratoLFO.waveTable.sinusoid();
        vibratoLFO.init(sampleRate/CORESAMPLER_CHUNKSIZE, 5.0f);
        restartVoiceLFO = false;
        volumeRamper.init(0.0f);
        tempGain = 0.0f;
    }

    void SamplerVoice::setGain(float gainDB) {
        // Convert gain in dB to linear scale
        gain = powf(10.0f, gainDB / 20.0f);
    }

    void SamplerVoice::setPan(float panValue) {
        // Clamp pan value between -1.0 (left) and 1.0 (right)
        pan = (panValue < -1.0f) ? -1.0f : (panValue > 1.0f) ? 1.0f : panValue;
    }

    uint32_t SamplerVoice::nextInstanceID = 1;

    uint32_t SamplerVoice::generateInstanceID() {
        return nextInstanceID++;
    }

    void SamplerVoice::start(unsigned note, float sampleRate, float frequency, float volume, SampleBuffer *buffer)
    {
        sampleBuffer = buffer;
        oscillator.indexPoint = buffer->startPoint;
        oscillator.increment = (buffer->sampleRate / sampleRate) * (frequency / buffer->noteFrequency);
        oscillator.multiplier = 1.0;
        oscillator.isLooping = buffer->isLooping;
        
        noteVolume = volume;
        ampEnvelope.start();
        volumeRamper.init(0.0f);
        
        samplingRate = sampleRate;
        leftFilter.updateSampleRate(double(samplingRate));
        rightFilter.updateSampleRate(double(samplingRate));
        filterEnvelope.start();

        pitchEnvelope.start();

        pitchEnvelopeSemitones = 0.0f;

        voiceLFOSemitones = 0.0f;

        glideSemitones = 0.0f;
        if (*glideSecPerOctave != 0.0f && noteFrequency != 0.0 && noteFrequency != frequency)
        {
            // prepare to glide
            glideSemitones = -12.0f * log2f(frequency / noteFrequency);
            if (fabsf(glideSemitones) < 0.01f) glideSemitones = 0.0f;
        }
        noteFrequency = frequency;
        noteNumber = note;
        instanceID = generateInstanceID();
        isInRelease = false;

        restartVoiceLFOIfNeeded();
    }
    
//    void SamplerVoice::restartNewNote(unsigned note, float sampleRate, float frequency, float volume, SampleBuffer *buffer)
//    {
//        samplingRate = sampleRate;
//        leftFilter.updateSampleRate(double(samplingRate));
//        rightFilter.updateSampleRate(double(samplingRate));
//
//        // Reinitialize oscillator for the new sample
//        oscillator.increment = (buffer->sampleRate / sampleRate) * (frequency / buffer->noteFrequency);
//        sampleBuffer = buffer;  // Reset buffer for new note
//
//        glideSemitones = 0.0f;
//        if (*glideSecPerOctave != 0.0f && noteFrequency != 0.0 && noteFrequency != frequency)
//        {
//            // Glide to the new note if needed
//            glideSemitones = -12.0f * log2f(frequency / noteFrequency);
//            if (fabsf(glideSemitones) < 0.01f) glideSemitones = 0.0f;
//        }
//
//        noteFrequency = frequency;
//        noteNumber = note;
//
//        ampEnvelope.restart();  // Retrigger amp envelope
//        filterEnvelope.restart();  // Retrigger filter envelope
//        pitchEnvelope.restart();  // Retrigger pitch envelope
//
//        noteVolume = volume;
//        restartVoiceLFOIfNeeded();  // Reset LFO if needed
//    }

void SamplerVoice::restartNewNoteLegato(unsigned note, float sampleRate, float frequency) {
    samplingRate = sampleRate;
    leftFilter.updateSampleRate(double(samplingRate));
    rightFilter.updateSampleRate(double(samplingRate));

    oscillator.increment = (sampleBuffer->sampleRate / sampleRate) * (frequency / sampleBuffer->noteFrequency);

    // Smooth glide to the new note frequency
    glideSemitones = 0.0f;
    if (*glideSecPerOctave != 0.0f && noteFrequency != 0.0 && noteFrequency != frequency) {
        glideSemitones = -12.0f * log2f(frequency / noteFrequency);
        if (fabsf(glideSemitones) < 0.01f) glideSemitones = 0.0f;
    }

    // Only adjust pitch, do not reset envelopes (legato behavior)
    noteFrequency = frequency;
    noteNumber = note;
}

//    void SamplerVoice::restartSameNote(float volume, SampleBuffer *buffer)
//    {
//        tempNoteVolume = noteVolume;
//        newSampleBuffer = buffer;
//        ampEnvelope.restart();
//        noteVolume = volume;
//        filterEnvelope.restart();
//        pitchEnvelope.restart();
//        restartVoiceLFOIfNeeded();
//    }
    
    void SamplerVoice::release(bool loopThruRelease)
    {
        isInRelease = true;
        if (!loopThruRelease) oscillator.isLooping = false;
        ampEnvelope.release();
        filterEnvelope.release();
        pitchEnvelope.release();
    }
    
    void SamplerVoice::stop()
    {
        noteNumber = -1;
        instanceID = 0;
        isInRelease = false;
        ampEnvelope.reset();
        volumeRamper.init(0.0f);
        filterEnvelope.reset();
        pitchEnvelope.reset();
    }

    bool SamplerVoice::prepToGetSamples(int sampleCount, float masterVolume, float pitchOffset,
                                        float cutoffMultiple, float keyTracking,
                                        float cutoffEnvelopeStrength, float cutoffEnvelopeVelocityScaling,
                                        float resLinear, float pitchADSRSemitones,
                                        float voiceLFODepthSemitones, float voiceLFOFrequencyHz,
                                        float globalLFOValue, float lfoTargetPitch, float lfoTargetGain, float lfoTargetFilter)
    {
        if (ampEnvelope.isIdle()) return true;

        if (ampEnvelope.isPreStarting())
        {
            tempGain = masterVolume * tempNoteVolume;
            volumeRamper.reinit(ampEnvelope.getSample(), sampleCount);
            // This can execute as part of the voice-stealing mechanism, and will be executed rarely.
            // To test, set MAX_POLYPHONY in CoreSampler.cpp to something small like 2 or 3.
            if (!ampEnvelope.isPreStarting())
            {
                tempGain = masterVolume * noteVolume;
                volumeRamper.reinit(ampEnvelope.getSample(), sampleCount);
                sampleBuffer = newSampleBuffer;
                oscillator.increment = (sampleBuffer->sampleRate / samplingRate) * (noteFrequency / sampleBuffer->noteFrequency);
                oscillator.indexPoint = sampleBuffer->startPoint;
                oscillator.isLooping = sampleBuffer->isLooping;
            }
        }
        else
        {
            tempGain = masterVolume * noteVolume;
            volumeRamper.reinit(ampEnvelope.getSample(), sampleCount);
        }

        if (*glideSecPerOctave != 0.0f && glideSemitones != 0.0f)
        {
            float seconds = sampleCount / samplingRate;
            float semitones = 12.0f * seconds / *glideSecPerOctave;
            if (glideSemitones < 0.0f)
            {
                glideSemitones += semitones;
                if (glideSemitones > 0.0f) glideSemitones = 0.0f;
            }
            else
            {
                glideSemitones -= semitones;
                if (glideSemitones < 0.0f) glideSemitones = 0.0f;
            }
        }

        float pitchCurveAmount = 1.0f; // >1 = faster curve, 0 < curve < 1 = slower curve - make this a parameter
        if (pitchCurveAmount < 0) { pitchCurveAmount = 0; }
        pitchEnvelopeSemitones = pow(pitchEnvelope.getSample(), pitchCurveAmount) * pitchADSRSemitones;

        vibratoLFO.setFrequency(voiceLFOFrequencyHz);
        voiceLFOSemitones = vibratoLFO.getSample() * voiceLFODepthSemitones;

        // Apply global LFO modulation if enabled
        if (lfoTargetPitch > 0.5f) {
            pitchOffset += globalLFOValue;  // Modulate pitch
        }
        if (lfoTargetGain > 0.5f) {
            tempGain += globalLFOValue;  // Modulate gain
        }

        float pitchOffsetModified = pitchOffset + glideSemitones + pitchEnvelopeSemitones + voiceLFOSemitones;
        oscillator.setPitchOffsetSemitones(pitchOffsetModified);

        // negative value of cutoffMultiple means filters are disabled
        if (cutoffMultiple < 0.0f)
        {
            isFilterEnabled = false;
        }
        else
        {
            isFilterEnabled = true;
            float noteHz = noteFrequency * powf(2.0f, (pitchOffsetModified) / 12.0f);
            float baseFrequency = MIDDLE_C_HZ + keyTracking * (noteHz - MIDDLE_C_HZ);
            float envStrength = ((1.0f - cutoffEnvelopeVelocityScaling) + cutoffEnvelopeVelocityScaling * noteVolume);
            double cutoffFrequency = baseFrequency * (1.0f + cutoffMultiple + cutoffEnvelopeStrength * envStrength * filterEnvelope.getSample());
            if (lfoTargetFilter > 0.5f) {
                cutoffFrequency += globalLFOValue * 2000;
            }
            leftFilter.setParameters(cutoffFrequency, resLinear);
            rightFilter.setParameters(cutoffFrequency, resLinear);
        }
        return false;
    }

    bool SamplerVoice::getSamples(int sampleCount, float* leftOutput, float* rightOutput) {
        for (int i = 0; i < sampleCount; i++) {
            // Apply gain
            float sampleGain = (tempGain + gain) * volumeRamper.getNextValue();
            float leftSample, rightSample;

            if (oscillator.getSamplePair(sampleBuffer, sampleCount, &leftSample, &rightSample, sampleGain)) return true;

            // Apply panning
            float panLeft = (pan <= 0.0f) ? 1.0f : (1.0f - pan);
            float panRight = (pan >= 0.0f) ? 1.0f : (1.0f + pan);

            float pannedLeftSample = leftSample * panLeft;
            float pannedRightSample = rightSample * panRight;

            if (isFilterEnabled) {
                *leftOutput++ += leftFilter.process(pannedLeftSample);
                *rightOutput++ += rightFilter.process(pannedRightSample);
            } else {
                *leftOutput++ += pannedLeftSample;
                *rightOutput++ += pannedRightSample;
            }
        }
        return false;
    }

    void SamplerVoice::restartVoiceLFOIfNeeded() {
        if (restartVoiceLFO || !hasStartedVoiceLFO) {
            vibratoLFO.phase = 0;
            hasStartedVoiceLFO = true;
        }
    }

}
