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

    void SamplerVoice::setGain(float gainDB)
    {
        // Convert gain in dB to linear scale
        gain = powf(10.0f, gainDB / 20.0f) - 1.0f;
    }

    void SamplerVoice::setPan(float panValue)
    {
        // Clamp pan value between -1.0 (left) and 1.0 (right)
        pan = (panValue < -1.0f) ? -1.0f : (panValue > 1.0f) ? 1.0f : panValue;
    }

    uint32_t SamplerVoice::nextInstanceID = 1;

    uint32_t SamplerVoice::generateInstanceID()
    {
        return nextInstanceID++;
    }

    void SamplerVoice::start(unsigned note, float sampleRate, float frequency, float volume, SampleBuffer *buffer)
    {
        sampleBuffer = buffer;

        // Random start offset with safe boundary checking
        float randomOffset = 0;
        if (*voiceStartOffsetRange > 0) {
            // Calculate maximum safe offset based on sample boundaries
            float maxSafeOffset = buffer->endPoint - buffer->startPoint;

            // If looping, also respect loop end point to ensure loop region is reached
            if (buffer->isLooping && buffer->loopEndPoint > buffer->startPoint) {
                maxSafeOffset = fminf(maxSafeOffset, buffer->loopEndPoint - buffer->startPoint);
            }

            // Clamp the range to what's actually safe
            float effectiveRange = fminf(*voiceStartOffsetRange, maxSafeOffset);

            if (effectiveRange > 0) {
                randomOffset = rand() % ((int)effectiveRange + 1);
            }
        }
        oscillator.indexPoint = buffer->startPoint + randomOffset;

        // Random detune for analog-style pitch variation
        float detuneFactor = 1.0f;
        if (*voiceDetuneRange > 0) {
            float centDetune = (rand() % ((int)(*voiceDetuneRange * 2) + 1)) - *voiceDetuneRange;
            detuneFactor = powf(2.0f, centDetune / 1200.0f);
        }

        // Random pan spread for stereo width
        if (*voicePanSpread > 0) {
            // Convert 0-100 spread to 0.0-1.0 range
            float spreadAmount = *voicePanSpread / 100.0f;
            // Random pan offset from -spreadAmount to +spreadAmount
            float randomPanOffset = (rand() % 201 - 100) / 100.0f * spreadAmount;
            // Add random offset to base pan and clamp to -1.0 to 1.0 range
            float newPan = pan + randomPanOffset;
            pan = (newPan < -1.0f) ? -1.0f : (newPan > 1.0f) ? 1.0f : newPan;
        }
        // If spread is 0, pan stays at its base value (from SFZ or API)

        // Calculate oscillator increment with safety checks to prevent division by zero
        if (sampleRate > 0.0f && buffer->noteFrequency > 0.0f) {
            oscillator.increment = (buffer->sampleRate / sampleRate) * (frequency / buffer->noteFrequency) * detuneFactor;
        } else {
            oscillator.increment = 1.0;  // Fallback to 1:1 playback
        }
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

    void SamplerVoice::restartNewNoteMono(unsigned note, float sampleRate, float frequency) {
        samplingRate = sampleRate;
        leftFilter.updateSampleRate(double(samplingRate));
        rightFilter.updateSampleRate(double(samplingRate));

        // Reset sample position with random start offset (same as start() method)
        float randomOffset = 0;
        if (*voiceStartOffsetRange > 0) {
            // Calculate maximum safe offset based on sample boundaries
            float maxSafeOffset = sampleBuffer->endPoint - sampleBuffer->startPoint;

            // If looping, also respect loop end point to ensure loop region is reached
            if (sampleBuffer->isLooping && sampleBuffer->loopEndPoint > sampleBuffer->startPoint) {
                maxSafeOffset = fminf(maxSafeOffset, sampleBuffer->loopEndPoint - sampleBuffer->startPoint);
            }

            // Clamp the range to what's actually safe
            float effectiveRange = fminf(*voiceStartOffsetRange, maxSafeOffset);

            if (effectiveRange > 0) {
                randomOffset = rand() % ((int)effectiveRange + 1);
            }
        }
        oscillator.indexPoint = sampleBuffer->startPoint + randomOffset;

        // Random detune for analog-style pitch variation
        float detuneFactor = 1.0f;
        if (*voiceDetuneRange > 0) {
            float centDetune = (rand() % ((int)(*voiceDetuneRange * 2) + 1)) - *voiceDetuneRange;
            detuneFactor = powf(2.0f, centDetune / 1200.0f);
        }

        // Random pan spread for stereo width
        if (*voicePanSpread > 0) {
            // Convert 0-100 spread to 0.0-1.0 range
            float spreadAmount = *voicePanSpread / 100.0f;
            // Random pan offset from -spreadAmount to +spreadAmount
            float randomPanOffset = (rand() % 201 - 100) / 100.0f * spreadAmount;
            // Add random offset to base pan and clamp to -1.0 to 1.0 range
            float newPan = pan + randomPanOffset;
            pan = (newPan < -1.0f) ? -1.0f : (newPan > 1.0f) ? 1.0f : newPan;
        }

        // Calculate oscillator increment with safety checks to prevent division by zero
        if (sampleRate > 0.0f && sampleBuffer->noteFrequency > 0.0f) {
            oscillator.increment = (sampleBuffer->sampleRate / sampleRate) * (frequency / sampleBuffer->noteFrequency) * detuneFactor;
        } else {
            oscillator.increment = 1.0;  // Fallback to 1:1 playback
        }
        oscillator.multiplier = 1.0;
        oscillator.isLooping = sampleBuffer->isLooping;
        glideSemitones = 0.0f;
        if (*glideSecPerOctave != 0.0f && noteFrequency != 0.0 && noteFrequency != frequency)
        {
            // prepare to glide
            glideSemitones = -12.0f * log2f(frequency / noteFrequency);
            if (fabsf(glideSemitones) < 0.01f) glideSemitones = 0.0f;
        }

        pitchEnvelopeSemitones = 0.0f;

        voiceLFOSemitones = 0.0f;

        noteFrequency = frequency;
        noteNumber = note;
        tempNoteVolume = noteVolume;
        newSampleBuffer = sampleBuffer;
        ampEnvelope.restart();
        filterEnvelope.restart();
        pitchEnvelope.restart();
        restartVoiceLFOIfNeeded();
    }

    void SamplerVoice::restartNewNoteLegato(unsigned note, float sampleRate, float frequency)
    {
        samplingRate = sampleRate;
        leftFilter.updateSampleRate(double(samplingRate));
        rightFilter.updateSampleRate(double(samplingRate));

        // Random detune for analog-style pitch variation
        float detuneFactor = 1.0f;
        if (*voiceDetuneRange > 0) {
            float centDetune = (rand() % ((int)(*voiceDetuneRange * 2) + 1)) - *voiceDetuneRange;
            detuneFactor = powf(2.0f, centDetune / 1200.0f);
        }

        // Random pan spread for stereo width
        if (*voicePanSpread > 0) {
            // Convert 0-100 spread to 0.0-1.0 range
            float spreadAmount = *voicePanSpread / 100.0f;
            // Random pan offset from -spreadAmount to +spreadAmount
            float randomPanOffset = (rand() % 201 - 100) / 100.0f * spreadAmount;
            // Add random offset to base pan and clamp to -1.0 to 1.0 range
            float newPan = pan + randomPanOffset;
            pan = (newPan < -1.0f) ? -1.0f : (newPan > 1.0f) ? 1.0f : newPan;
        }

        // Calculate oscillator increment with safety checks to prevent division by zero
        if (sampleRate > 0.0f && sampleBuffer->noteFrequency > 0.0f) {
            oscillator.increment = (sampleBuffer->sampleRate / sampleRate) * (frequency / sampleBuffer->noteFrequency) * detuneFactor;
        } else {
            oscillator.increment = 1.0;  // Fallback to 1:1 playback
        }
        oscillator.multiplier = 1.0;
        oscillator.isLooping = sampleBuffer->isLooping;

        // Smooth glide to the new note frequency
        glideSemitones = 0.0f;
        if (*glideSecPerOctave != 0.0f && noteFrequency != 0.0 && noteFrequency != frequency)
        {
            glideSemitones = -12.0f * log2f(frequency / noteFrequency);
            if (fabsf(glideSemitones) < 0.01f) glideSemitones = 0.0f;
        }

        // Only adjust pitch, do not reset envelopes (legato behavior)
        noteFrequency = frequency;
        noteNumber = note;
    }

    void SamplerVoice::release(bool loopThruRelease)
    {
        isInRelease = true;
        if (!loopThruRelease)
        {
            oscillator.isLooping = false;
        }
        ampEnvelope.release();
        filterEnvelope.release();
        pitchEnvelope.release();
    }

    void SamplerVoice::stop()
    {
        noteNumber = -1;
        instanceID = 0;
        unisonGroupID = 0;
        unisonIndex = 0;
        totalUnisonVoices = 1;
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

                // Only reset oscillator position if we're actually changing buffers
                // Otherwise preserve the indexPoint that was set (with random offset)
                if (sampleBuffer != newSampleBuffer)
                {
                    sampleBuffer = newSampleBuffer;
                    // Calculate oscillator increment with safety checks to prevent division by zero
                    if (samplingRate > 0.0f && sampleBuffer->noteFrequency > 0.0f) {
                        oscillator.increment = (sampleBuffer->sampleRate / samplingRate) * (noteFrequency / sampleBuffer->noteFrequency);
                    } else {
                        oscillator.increment = 1.0;  // Fallback to 1:1 playback
                    }
                    oscillator.indexPoint = sampleBuffer->startPoint;
                    oscillator.isLooping = sampleBuffer->isLooping;
                }
            }
        }
        else
        {
            tempGain = masterVolume * noteVolume;
            volumeRamper.reinit(ampEnvelope.getSample(), sampleCount);
        }
        
        if (*glideSecPerOctave != 0.0f && glideSemitones != 0.0f && samplingRate > 0.0f)
        {
            float seconds = sampleCount / samplingRate;

            // Calculate semitone change based on the distance remaining (larger distance = faster change)
            float semitoneChange = 12.0f * seconds / *glideSecPerOctave;

            // Apply non-linear scaling based on the remaining distance (larger distance -> faster movement)
            //This can be removed if you want a fixed time adjustment
            semitoneChange *= fabsf(glideSemitones);  // Scale by the remaining distance

            if (glideSemitones < 0.0f)
            {
                glideSemitones += semitoneChange;  // Move toward the target note
                if (glideSemitones > 0.0f) glideSemitones = 0.0f;  // Stop at the target note
            }
            else
            {
                glideSemitones -= semitoneChange;  // Move toward the target note
                if (glideSemitones < 0.0f) glideSemitones = 0.0f;  // Stop at the target note
            }
        }

        float pitchCurveAmount = 1.0f; // >1 = faster curve, 0 < curve < 1 = slower curve - make this a parameter
        if (pitchCurveAmount < 0) { pitchCurveAmount = 0; }
        pitchEnvelopeSemitones = pow(pitchEnvelope.getSample(), pitchCurveAmount) * pitchADSRSemitones;

        vibratoLFO.setFrequency(voiceLFOFrequencyHz);
        voiceLFOSemitones = vibratoLFO.getSample() * voiceLFODepthSemitones;

        // Apply global LFO modulation if enabled
        if (lfoTargetPitch > 0.5f)
        {
            pitchOffset += globalLFOValue * 0.5;  // Modulate pitch
        }
        if (lfoTargetGain > 0.5f)
        {
            tempGain += globalLFOValue * 0.5;  // Modulate gain
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

            // Calculate base cutoff frequency with safe multiplier
            // Clamp the multiplier to prevent negative or extreme values
            float cutoffMultiplier = 1.0f + cutoffMultiple + cutoffEnvelopeStrength * envStrength * filterEnvelope.getSample();
            cutoffMultiplier = std::max(0.05f, std::min(cutoffMultiplier, 100.0f));  // Clamp multiplier to reasonable range

            double cutoffFrequency = baseFrequency * cutoffMultiplier;

            // Apply LFO modulation if enabled
            if (lfoTargetFilter > 0.5f)
            {
                // Limit how much the LFO can offset the cutoff
                float maxLFOFilterOffset = 2000.0f;
                float lfoFilterMod = globalLFOValue * maxLFOFilterOffset;
                cutoffFrequency += lfoFilterMod;
            }

            // Clamp cutoff frequency to safe range to prevent filter instability/crashes
            // Min: 12Hz (filter's internal minimum), Max: Nyquist frequency (samplingRate/2)
            float minCutoffHz = 12.0f;
            float maxCutoffHz = samplingRate * 0.45f;  // 45% of sample rate to stay well below Nyquist
            cutoffFrequency = std::max((double)minCutoffHz, std::min(cutoffFrequency, (double)maxCutoffHz));

            leftFilter.setParameters(cutoffFrequency, resLinear);
            rightFilter.setParameters(cutoffFrequency, resLinear);
        }
        return false;
    }

    bool SamplerVoice::getSamples(int sampleCount, float* leftOutput, float* rightOutput) {
        for (int i = 0; i < sampleCount; i++) {
            // Apply gain
            float sampleGain = tempGain * (gain + 1.0f) * volumeRamper.getNextValue();
            float leftSample, rightSample;

            if (oscillator.getSamplePair(sampleBuffer, sampleCount, &leftSample, &rightSample, sampleGain)) return true;

            // Apply panning
            float panLeft = (pan <= 0.0f) ? 1.0f : (1.0f - pan);
            float panRight = (pan >= 0.0f) ? 1.0f : (1.0f + pan);

            float pannedLeftSample = leftSample * panLeft;
            float pannedRightSample = rightSample * panRight;

            if (isFilterEnabled)
            {
                *leftOutput++ += leftFilter.process(pannedLeftSample);
                *rightOutput++ += rightFilter.process(pannedRightSample);
            }
            else
            {
                *leftOutput++ += pannedLeftSample;
                *rightOutput++ += pannedRightSample;
            }
        }
        return false;
    }

    void SamplerVoice::restartVoiceLFOIfNeeded()
    {
        if (restartVoiceLFO || !hasStartedVoiceLFO)
        {
            vibratoLFO.phase = 0;
            hasStartedVoiceLFO = true;
        }
    }
}
