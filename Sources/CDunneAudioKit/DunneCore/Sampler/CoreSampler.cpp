// Copyright AudioKit. All Rights Reserved.

#include "CoreSampler.h"
#include "SamplerVoice.h"
#include "FunctionTable.h"
#include "SustainPedalLogic.h"

#include <math.h>
#include <list>
#include <iostream>

// Number of voices
#define MAX_POLYPHONY 64

// MIDI offers 128 distinct note numbers
#define MIDI_NOTENUMBERS 128

// Convert MIDI note to Hz for 12-tone equal temperament
#define NOTE_HZ(midiNoteNumber) (440.0f * pow(2.0f, ((midiNoteNumber) - 69.0f) / 12.0f))

struct CoreSampler::InternalData
{
    // List of (pointers to) all loaded samples
    std::list<DunneCore::KeyMappedSampleBuffer*> sampleBufferList;
    
    // Maps MIDI note numbers to "closest" samples (all velocity layers)
    std::list<DunneCore::KeyMappedSampleBuffer*> keyMap[MIDI_NOTENUMBERS];
    
    // Envelope parameters
    DunneCore::AHDSHREnvelopeParameters ampEnvelopeParameters;
    DunneCore::ADSREnvelopeParameters filterEnvelopeParameters;
    DunneCore::ADSREnvelopeParameters pitchEnvelopeParameters;
    
    // Voice resources
    DunneCore::SamplerVoice voice[MAX_POLYPHONY];
    
    // Shared vibrato and global LFOs
    DunneCore::FunctionTableOscillator vibratoLFO;
    DunneCore::FunctionTableOscillator globalLFO;
    
    // Sustain pedal logic
    DunneCore::SustainPedalLogic pedalLogic;
    
    // Tuning table
    float tuningTable[128];
};

CoreSampler::CoreSampler()
: currentSampleRate(44100.0f)    // sensible guess
, isKeyMapValid(false)
, isFilterEnabled(false)
, restartVoiceLFO(false)
, overallGain(0.0f)
, overallPan(0.0f)
, masterVolume(1.0f)
, pitchOffset(0.0f)
, vibratoDepth(0.0f)
, vibratoFrequency(5.0f)
, voiceVibratoDepth(0.0f)
, voiceVibratoFrequency(5.0f)
, glideRate(0.0f)   // 0 sec/octave means "no glide"
, lfoRate(5.0f)
, lfoDepth(0.0f)
, lfoTargetPitchToggle(0.0f)
, lfoTargetGainToggle(0.0f)
, lfoTargetFilterToggle(0.0f)
, isMonophonic(false)
, isLegato(false)
, portamentoRate(1.0f)
, cutoffMultiple(4.0f)
, keyTracking(1.0f)
, cutoffEnvelopeStrength(20.0f)
, filterEnvelopeVelocityScaling(0.0f)
, linearResonance(0.5f)
, pitchADSRSemitones(0.0f)
, loopThruRelease(true)
, stoppingAllVoices(false)
, data(new InternalData)
{
    DunneCore::SamplerVoice *pVoice = data->voice;
    for (int i=0; i < MAX_POLYPHONY; i++, pVoice++)
    {
        pVoice->ampEnvelope.pParameters = &data->ampEnvelopeParameters;
        pVoice->filterEnvelope.pParameters = &data->filterEnvelopeParameters;
        pVoice->pitchEnvelope.pParameters = &data->pitchEnvelopeParameters;
        pVoice->noteFrequency = 0.0f;
        pVoice->glideSecPerOctave = &glideRate;
    }
    
    for (int i=0; i < 128; i++)
        data->tuningTable[i] = NOTE_HZ(i);
}

CoreSampler::~CoreSampler()
{
    unloadAllSamples();
}

int CoreSampler::init(double sampleRate)
{
    currentSampleRate = (float)sampleRate;
    data->ampEnvelopeParameters.updateSampleRate((float)(sampleRate/CORESAMPLER_CHUNKSIZE));
    data->filterEnvelopeParameters.updateSampleRate((float)(sampleRate/CORESAMPLER_CHUNKSIZE));
    data->pitchEnvelopeParameters.updateSampleRate((float)(sampleRate/CORESAMPLER_CHUNKSIZE));
    data->vibratoLFO.waveTable.sinusoid();
    data->vibratoLFO.init(sampleRate/CORESAMPLER_CHUNKSIZE, 5.0f);
    data->globalLFO.waveTable.sinusoid();
    data->globalLFO.init(sampleRate / CORESAMPLER_CHUNKSIZE, lfoRate);
    
    for (int i=0; i<MAX_POLYPHONY; i++)
        data->voice[i].init(sampleRate);
    return 0;   // no error
}

void CoreSampler::deinit()
{
    // Optional cleanup if needed
}

void CoreSampler::unloadAllSamples()
{
    isKeyMapValid = false;
    for (DunneCore::KeyMappedSampleBuffer *pBuf : data->sampleBufferList)
        delete pBuf;
    data->sampleBufferList.clear();
    for (int i=0; i < MIDI_NOTENUMBERS; i++)
        data->keyMap[i].clear();
}

void CoreSampler::loadSampleData(SampleDataDescriptor& sdd)
{
    DunneCore::KeyMappedSampleBuffer *pBuf = new DunneCore::KeyMappedSampleBuffer();
    pBuf->minimumNoteNumber = sdd.sampleDescriptor.minimumNoteNumber;
    pBuf->maximumNoteNumber = sdd.sampleDescriptor.maximumNoteNumber;
    pBuf->minimumVelocity = sdd.sampleDescriptor.minimumVelocity;
    pBuf->maximumVelocity = sdd.sampleDescriptor.maximumVelocity;
    pBuf->volume = sdd.sampleDescriptor.volume;
    pBuf->pan = sdd.sampleDescriptor.pan;
    
    data->sampleBufferList.push_back(pBuf);
    
    // Rest of the code
    pBuf->init(sdd.sampleRate, sdd.channelCount, sdd.sampleCount);
    float *pData = sdd.data;
    if (sdd.isInterleaved)
    {
        for (int i = 0; i < sdd.sampleCount; i++)
        {
            pBuf->setData(i, *pData++);
            if (sdd.channelCount > 1) pBuf->setData(sdd.sampleCount + i, *pData++);
        }
    }
    else
    {
        for (int i = 0; i < sdd.channelCount * sdd.sampleCount; i++)
        {
            pBuf->setData(i, *pData++);
        }
    }
    pBuf->noteNumber = sdd.sampleDescriptor.noteNumber;
    pBuf->tune = 0; //Reset our tune value but apply it to the frequency
    pBuf->noteFrequency = sdd.sampleDescriptor.noteFrequency * powf(2.0f, sdd.sampleDescriptor.tune / 1200.0f);
    
    // Handle rare case where loopEndPoint is 0 (due to being uninitialized)
    if (sdd.sampleDescriptor.loopEndPoint == 0.0f)
        sdd.sampleDescriptor.loopEndPoint = float(sdd.sampleCount - 1);
    
    if (sdd.sampleDescriptor.startPoint > 0.0f) pBuf->startPoint = sdd.sampleDescriptor.startPoint;
    if (sdd.sampleDescriptor.endPoint > 0.0f)   pBuf->endPoint = sdd.sampleDescriptor.endPoint;
    
    pBuf->isLooping = sdd.sampleDescriptor.isLooping;
    if (pBuf->isLooping)
    {
        // loopStartPoint, loopEndPoint are usually sample indices, but values 0.0-1.0
        // are interpreted as fractions of the total sample length.
        if (sdd.sampleDescriptor.loopStartPoint > 1.0f) pBuf->loopStartPoint = sdd.sampleDescriptor.loopStartPoint;
        else pBuf->loopStartPoint = pBuf->endPoint * sdd.sampleDescriptor.loopStartPoint;
        if (sdd.sampleDescriptor.loopEndPoint > 1.0f) pBuf->loopEndPoint = sdd.sampleDescriptor.loopEndPoint;
        else pBuf->loopEndPoint = pBuf->endPoint * sdd.sampleDescriptor.loopEndPoint;
        
        // Clamp loop endpoints to valid range
        if (pBuf->loopStartPoint < pBuf->startPoint) pBuf->loopStartPoint = pBuf->startPoint;
        if (pBuf->loopEndPoint > pBuf->endPoint) pBuf->loopEndPoint = pBuf->endPoint;
    }
}

std::vector<DunneCore::KeyMappedSampleBuffer *> CoreSampler::lookupSamples(unsigned noteNumber, unsigned velocity)
{
    std::vector<DunneCore::KeyMappedSampleBuffer *> buffers;
    
    for (DunneCore::KeyMappedSampleBuffer* pBuf : data->keyMap[noteNumber])
    {
        // Check velocity and add the buffer to the list
        if (velocity >= pBuf->minimumVelocity && velocity <= pBuf->maximumVelocity)
        {
            buffers.push_back(pBuf);  // Add based on velocity range
        }
    }
    
    return buffers;
}

void CoreSampler::setNoteFrequency(int noteNumber, float noteFrequency)
{
    data->tuningTable[noteNumber] = noteFrequency;
}

// re-compute keyMap[] so every MIDI note number is automatically mapped to the sample buffer
// closest in pitch
void CoreSampler::buildSimpleKeyMap()
{
    // clear out the old mapping entirely
    isKeyMapValid = false;
    for (int i=0; i < MIDI_NOTENUMBERS; i++)
    {
        data->keyMap[i].clear();
    }

    for (int nn=0; nn < MIDI_NOTENUMBERS; nn++)
    {
        float noteFreq = data->tuningTable[nn];
        
        // scan loaded samples to find the minimum distance to note nn
        float minDistance = 1000000.0f;
        for (DunneCore::KeyMappedSampleBuffer *pBuf : data->sampleBufferList)
        {
            float distance = fabsf(NOTE_HZ(pBuf->noteNumber) - noteFreq);
            if (distance < minDistance)
            {
                minDistance = distance;
            }
        }
        
        // scan again to add only samples at this distance to the list for note nn
        for (DunneCore::KeyMappedSampleBuffer *pBuf : data->sampleBufferList)
        {
            float distance = fabsf(NOTE_HZ(pBuf->noteNumber) - noteFreq);
            if (distance == minDistance)
            {
                data->keyMap[nn].push_back(pBuf);
            }
        }
    }
    isKeyMapValid = true;
}

// rebuild keyMap based on explicit mapping data in samples
void CoreSampler::buildKeyMap(void)
{
    // clear out the old mapping entirely
    isKeyMapValid = false;
    for (int i=0; i < MIDI_NOTENUMBERS; i++)
    {
        data->keyMap[i].clear();
    }

    for (int nn=0; nn < MIDI_NOTENUMBERS; nn++)
    {
        float noteFreq = data->tuningTable[nn];
        for (DunneCore::KeyMappedSampleBuffer *pBuf : data->sampleBufferList)
        {
            float minFreq = NOTE_HZ(pBuf->minimumNoteNumber);
            float maxFreq = NOTE_HZ(pBuf->maximumNoteNumber);
            if (noteFreq >= minFreq && noteFreq <= maxFreq)
                data->keyMap[nn].push_back(pBuf);
        }
    }
    isKeyMapValid = true;
}

DunneCore::SamplerVoice *CoreSampler::voicePlayingNote(unsigned noteNumber)
{
    for (int i=0; i < MAX_POLYPHONY; i++)
    {
        DunneCore::SamplerVoice *pVoice = &data->voice[i];
        if (pVoice->noteNumber == noteNumber) return pVoice;
    }
    return 0;
}

void CoreSampler::resetLFOStart()
{
    data->vibratoLFO.resetSync(); // Reset vibrato LFO to start phase
    data->globalLFO.resetSync();  // Reset global LFO to start phase
}

void CoreSampler::addHeldNote(unsigned noteNumber)
{
    // Remove the note if it already exists, to ensure proper stacking order
    heldNotes.erase(std::remove(heldNotes.begin(), heldNotes.end(), noteNumber), heldNotes.end());
    // Add the new note at the back of the list
    heldNotes.push_back(noteNumber);
}

void CoreSampler::removeHeldNote(unsigned noteNumber)
{
    heldNotes.erase(std::remove(heldNotes.begin(), heldNotes.end(), noteNumber), heldNotes.end());
}

unsigned CoreSampler::getLastHeldNote()
{
    if (!heldNotes.empty()) {
        return heldNotes.back();
    }
    return -1;
}

void CoreSampler::playNote(unsigned noteNumber, unsigned velocity)
{
    bool anotherKeyWasDown = data->pedalLogic.isAnyKeyDown();

    // Send previous note into release state
    if (!isLegato) {
        stop(noteNumber, false);
    }

    data->pedalLogic.keyDownAction(noteNumber); // Ensure we mark this key as held
    
    // Get all samples (regions) corresponding to this note number and velocity
    auto buffers = lookupSamples(noteNumber, velocity);
    if (buffers.empty()) return;
    
    // Track this note as being held
    addHeldNote(noteNumber);
    
    // Debug Message
    // std::cout << "playNote called for note: " << noteNumber << std::endl;
    
    if (isMonophonic)
    {
        if (anotherKeyWasDown)
        {
            // Legato mode: glide to the new note without restarting envelopes
            for (auto* pBuf : buffers)
            {
                for (int i = 0; i < MAX_POLYPHONY; i++)
                {
                    DunneCore::SamplerVoice* pVoice = &data->voice[i];
                    if (pVoice->noteNumber >= 0 && pVoice->sampleBuffer == pBuf)
                    {  // Reuse the existing voice for the new note
                        if (isLegato)
                        {
                            pVoice->restartNewNoteLegato(noteNumber, currentSampleRate, data->tuningTable[noteNumber]);
                        }
                        else
                        {
                            pVoice->restartNewNoteMono(noteNumber, currentSampleRate, data->tuningTable[noteNumber]);
                        }
                    }
                }
            }
        }
        else
        {
            // no other key was down: stop the current note and start the new one
            stopAllVoicesMonophonic();
            play(noteNumber, velocity, anotherKeyWasDown);
        }
    }
    else
    {
        // Polyphonic mode: play all voices as per current logic
        play(noteNumber, velocity, anotherKeyWasDown);
    }
}

void CoreSampler::stopNote(unsigned noteNumber, bool immediate)
{
    // Get the last held note before removing the current note
    unsigned lastNote = getLastHeldNote();
    
    // Remove this note from the held notes list
    removeHeldNote(noteNumber);
    
    // Tell the sustain pedal logic that this key is being released
    if (immediate || !data->pedalLogic.keyUpAction(noteNumber))
    {
        // Stop the note normally
        auto buffers = lookupSamples(noteNumber, 0); // Velocity is not relevant for stopping
        
        // Ensure we stop each region (buffer) only once
        for (auto* pBuf : buffers)
        {
            if (pBuf) stop(noteNumber, immediate);
        }
    }
    
    if (isMonophonic && !immediate)
    {
        if (data->pedalLogic.isAnyKeyDown() == false)
        {
            for (int i = 0; i < MAX_POLYPHONY; i++)
            {
                data->voice[i].release(loopThruRelease);  // Stop each voice
            }
        }
        else
        {
            // In legato mode, smoothly glide to the new last held note without retriggering envelopes
            unsigned newLastNote = getLastHeldNote();
            if (isLegato && newLastNote != (unsigned)-1 && newLastNote != noteNumber)
            {
                playNote(newLastNote, 127);  // Glide to the new last held note
            }
            else if (!isLegato && newLastNote != lastNote && newLastNote != (unsigned)-1)
            {
                // In non-legato mode, retrigger the new last held note
                playNote(newLastNote, 127);
            }
        }
    }
    
    //  Tell the sustain pedal logic that this key is being released
    if (immediate || data->pedalLogic.keyUpAction(noteNumber))
    {
        // Stop the note normally
        auto buffers = lookupSamples(noteNumber, 0); // Velocity is not relevant for stopping
        
        // Ensure we stop each region (buffer) only once
        for (auto* pBuf : buffers)
        {
            if (pBuf) stop(noteNumber, immediate);
        }
    }
}

void CoreSampler::play(unsigned noteNumber, unsigned velocity, bool anotherKeyWasDown)
{
    if (stoppingAllVoices) return;

    float noteFrequency = data->tuningTable[noteNumber];
    auto samples = lookupSamples(noteNumber, velocity);
    if (samples.empty()) return;

    for (auto* pBuf : samples)
    {
        float detuneFactor = powf(2.0f, pBuf->tune / 1200.0f);
        float detunedFrequency = noteFrequency * detuneFactor;

        DunneCore::SamplerVoice* pVoice = nullptr;

        // Find an inactive voice or prepare to steal one if all voices are active
        for (int i = 0; i < MAX_POLYPHONY; i++)
        {
            if (data->voice[i].noteNumber < 0) {
                pVoice = &data->voice[i];
                break;
            }
        }

        // If all voices are active, implement voice-stealing
        if (!pVoice)
        {
            // Choose the oldest note (first element in activeNotes)
            auto& oldestNote = activeNotes.front();
            uint32_t instanceID = std::get<1>(oldestNote);

            for (int i = 0; i < MAX_POLYPHONY; i++)
            {
                if (data->voice[i].instanceID == instanceID)
                {
                    pVoice = &data->voice[i];
                    break;
                }
            }
            pVoice->stop();  // Stop the oldest voice to free up a slot
            activeNotes.erase(activeNotes.begin());  // Remove the oldest note from activeNotes
        }

        // Start the selected voice
        if (pVoice)
        {
            pVoice->start(noteNumber, currentSampleRate, detunedFrequency, velocity / 127.0f, pBuf);
            pVoice->setGain(pBuf->volume);
            pVoice->setPan(pBuf->pan);

            lastPlayedNoteNumber = noteNumber;
            activeNotes.push_back({noteNumber, pVoice->instanceID, false});  // Add the new note to the back
        }
    }
}


void CoreSampler::stop(unsigned noteNumber, bool immediate)
{
    // Loop through active notes to find the right instance to stop

    for (auto &entry : activeNotes)
    {
        if (std::get<0>(entry) == noteNumber) // Check if noteNumber matches
        {
            uint32_t instanceID = std::get<1>(entry);
            bool isInRelease = std::get<2>(entry);

            // Find the corresponding voice for this instanceID
            DunneCore::SamplerVoice *pVoice = nullptr;
            for (int i = 0; i < MAX_POLYPHONY; i++)
            {
                if (data->voice[i].noteNumber == noteNumber && data->voice[i].instanceID == instanceID)
                {
                    pVoice = &data->voice[i];
                    break;
                }
            }

            if (!pVoice) continue;

            if (immediate)
            {
                // Immediately stop the voice and remove from activeNotes
                pVoice->stop();
                activeNotes.erase(std::remove(activeNotes.begin(), activeNotes.end(), entry), activeNotes.end());
                return;
            }
            else if (!isInRelease)
            {
                // Release the voice if it's not already in the release phase
                pVoice->release(loopThruRelease);
                std::get<2>(entry) = true; // Mark as in release
                return;
            }
        }
    }
}

void CoreSampler::stopAllVoicesMonophonic() {
    // Stop all voices and remove them from active notes
    for (int i = 0; i < MAX_POLYPHONY; i++)
    {
        data->voice[i].stop();  // Stop each voice
    }
}

void CoreSampler::sustainPedal(bool down)
{
    if (down)
    {
        data->pedalLogic.pedalDown();
    }
    else
    {
        for (int nn = 0; nn < MIDI_NOTENUMBERS; nn++)
        {
                    // Check if this note is currently held by the pedal
            if (data->pedalLogic.isNoteSustaining(nn))
                    {
                        // Loop through all voices to find instances of this note that are not in release
                        for (int i = 0; i < MAX_POLYPHONY; i++)
                        {
                            DunneCore::SamplerVoice &voice = data->voice[i];
                            // Check if this voice is playing the note and not in its release state
                            if (voice.noteNumber == nn && !voice.isInRelease)
                            {
                                stop(nn, false); // Stop the voice only if it's not already releasing
                            }
                        }
                    }
                }
        data->pedalLogic.pedalUp();
    }
}

void CoreSampler::stopAllVoices()
{
    // Lock out starting any new notes, and tell Render() to stop all active notes
    stoppingAllVoices = true;
    heldNotes.clear(); // Ensure all held notes are reset
    
    // Wait until Render() has killed all active notes
    bool noteStillSounding = true;
    while (noteStillSounding)
    {
        noteStillSounding = false;
        for (int i=0; i < MAX_POLYPHONY; i++)
        {
            if (data->voice[i].noteNumber >= 0)
            {
                noteStillSounding = true;
            }
        }
    }
}

void CoreSampler::restartVoices()
{
    // Allow starting new notes again
    stoppingAllVoices = false;
}

void CoreSampler::render(unsigned channelCount, unsigned sampleCount, float *outBuffers[])
{
    float *pOutLeft = outBuffers[0];
    float *pOutRight = outBuffers[1];

    // Clear output buffers
    for (unsigned i = 0; i < sampleCount; i++)
    {
        pOutLeft[i] = 0.0f;
        pOutRight[i] = 0.0f;
    }
    
    float cutoffMul = isFilterEnabled ? cutoffMultiple : -1.0f;

    // Set the global LFO frequency
    data->globalLFO.setFrequency(lfoRate);
    
    // Get the current value from the global LFO
    float globalLFOValue = data->globalLFO.getSample() * lfoDepth;

    // Update vibrato LFO frequency
    data->vibratoLFO.setFrequency(vibratoFrequency);

    // Modify pitch offset with vibrato
    float pitchDev = this->pitchOffset + vibratoDepth * data->vibratoLFO.getSample();

    // Process each voice (polyphonic voices)
    for (int i = 0; i < MAX_POLYPHONY; i++)
    {
        DunneCore::SamplerVoice *pVoice = &data->voice[i];
        if (pVoice->noteNumber >= 0)
        {
            // Call the existing voice rendering logic
            bool shouldStop = pVoice->prepToGetSamples(sampleCount, masterVolume, pitchDev, cutoffMul,
                                                       keyTracking, cutoffEnvelopeStrength, filterEnvelopeVelocityScaling,
                                                       linearResonance, pitchADSRSemitones, voiceVibratoDepth, voiceVibratoFrequency,
                                                       globalLFOValue, lfoTargetPitchToggle, lfoTargetGainToggle, lfoTargetFilterToggle);
            // If the voice is done, stop it
            if (shouldStop)
            {
                stopNote(pVoice->noteNumber, true);
            }
            else
            {
                pVoice->getSamples(sampleCount, pOutLeft, pOutRight);
            }
        }
    }
    
    // Apply overall gain and pan (after processing voices)
    float overallGainLinear = powf(10.0f, overallGain / 20.0f);
    float leftPan = (overallPan <= 0.0f) ? 1.0f : (1.0f - overallPan);
    float rightPan = (overallPan >= 0.0f) ? 1.0f : (1.0f + overallPan);
    
    for (unsigned i = 0; i < sampleCount; i++)
    {
        float leftValue = pOutLeft[i] * overallGainLinear * leftPan;
        float rightValue = pOutRight[i] * overallGainLinear * rightPan;
        pOutLeft[i] = leftValue;
        pOutRight[i] = rightValue;
    }
}


void  CoreSampler::setADSRAttackDurationSeconds(float value) __attribute__((no_sanitize("thread")))
{
    data->ampEnvelopeParameters.setAttackDurationSeconds(value);
    for (int i = 0; i < MAX_POLYPHONY; i++) data->voice[i].updateAmpAdsrParameters();
}

float CoreSampler::getADSRAttackDurationSeconds(void)
{
    return data->ampEnvelopeParameters.getAttackDurationSeconds();
}

void  CoreSampler::setADSRHoldDurationSeconds(float value)
{
    data->ampEnvelopeParameters.setHoldDurationSeconds(value);
    for (int i = 0; i < MAX_POLYPHONY; i++) data->voice[i].updateAmpAdsrParameters();
}

float CoreSampler::getADSRHoldDurationSeconds(void)
{
    return data->ampEnvelopeParameters.getHoldDurationSeconds();
}

void  CoreSampler::setADSRDecayDurationSeconds(float value)
{
    data->ampEnvelopeParameters.setDecayDurationSeconds(value);
    for (int i = 0; i < MAX_POLYPHONY; i++) data->voice[i].updateAmpAdsrParameters();
}

float CoreSampler::getADSRDecayDurationSeconds(void)
{
    return data->ampEnvelopeParameters.getDecayDurationSeconds();
}

void  CoreSampler::setADSRSustainFraction(float value)
{
    data->ampEnvelopeParameters.sustainFraction = value;
    for (int i = 0; i < MAX_POLYPHONY; i++) data->voice[i].updateAmpAdsrParameters();
}

float CoreSampler::getADSRSustainFraction(void)
{
    return data->ampEnvelopeParameters.sustainFraction;
}

void  CoreSampler::setADSRReleaseHoldDurationSeconds(float value)
{
    data->ampEnvelopeParameters.setReleaseHoldDurationSeconds(value);
    for (int i = 0; i < MAX_POLYPHONY; i++) data->voice[i].updateAmpAdsrParameters();
}

float CoreSampler::getADSRReleaseHoldDurationSeconds(void)
{
    return data->ampEnvelopeParameters.getReleaseHoldDurationSeconds();
}

void  CoreSampler::setADSRReleaseDurationSeconds(float value)
{
    data->ampEnvelopeParameters.setReleaseDurationSeconds(value);
    for (int i = 0; i < MAX_POLYPHONY; i++) data->voice[i].updateAmpAdsrParameters();
}

float CoreSampler::getADSRReleaseDurationSeconds(void)
{
    return data->ampEnvelopeParameters.getReleaseDurationSeconds();
}

void  CoreSampler::setFilterAttackDurationSeconds(float value)
{
    data->filterEnvelopeParameters.setAttackDurationSeconds(value);
    for (int i = 0; i < MAX_POLYPHONY; i++) data->voice[i].updateFilterAdsrParameters();
}

float CoreSampler::getFilterAttackDurationSeconds(void)
{
    return data->filterEnvelopeParameters.getAttackDurationSeconds();
}

void  CoreSampler::setFilterDecayDurationSeconds(float value)
{
    data->filterEnvelopeParameters.setDecayDurationSeconds(value);
    for (int i = 0; i < MAX_POLYPHONY; i++) data->voice[i].updateFilterAdsrParameters();
}

float CoreSampler::getFilterDecayDurationSeconds(void)
{
    return data->filterEnvelopeParameters.getDecayDurationSeconds();
}

void  CoreSampler::setFilterSustainFraction(float value)
{
    data->filterEnvelopeParameters.sustainFraction = value;
    for (int i = 0; i < MAX_POLYPHONY; i++) data->voice[i].updateFilterAdsrParameters();
}

float CoreSampler::getFilterSustainFraction(void)
{
    return data->filterEnvelopeParameters.sustainFraction;
}

void  CoreSampler::setFilterReleaseDurationSeconds(float value)
{
    data->filterEnvelopeParameters.setReleaseDurationSeconds(value);
    for (int i = 0; i < MAX_POLYPHONY; i++) data->voice[i].updateFilterAdsrParameters();
}

float CoreSampler::getFilterReleaseDurationSeconds(void)
{
    return data->filterEnvelopeParameters.getReleaseDurationSeconds();
}

void  CoreSampler::setPitchAttackDurationSeconds(float value)
{
    data->pitchEnvelopeParameters.setAttackDurationSeconds(value);
    for (int i = 0; i < MAX_POLYPHONY; i++) data->voice[i].updatePitchAdsrParameters();
}

float CoreSampler::getPitchAttackDurationSeconds(void)
{
    return data->pitchEnvelopeParameters.getAttackDurationSeconds();
}

void  CoreSampler::setPitchDecayDurationSeconds(float value)
{
    data->pitchEnvelopeParameters.setDecayDurationSeconds(value);
    for (int i = 0; i < MAX_POLYPHONY; i++) data->voice[i].updatePitchAdsrParameters();
}

float CoreSampler::getPitchDecayDurationSeconds(void)
{
    return data->pitchEnvelopeParameters.getDecayDurationSeconds();
}

void  CoreSampler::setPitchSustainFraction(float value)
{
    data->pitchEnvelopeParameters.sustainFraction = value;
    for (int i = 0; i < MAX_POLYPHONY; i++) data->voice[i].updatePitchAdsrParameters();
}

float CoreSampler::getPitchSustainFraction(void)
{
    return data->pitchEnvelopeParameters.sustainFraction;
}

void  CoreSampler::setPitchReleaseDurationSeconds(float value)
{
    data->pitchEnvelopeParameters.setReleaseDurationSeconds(value);
    for (int i = 0; i < MAX_POLYPHONY; i++) data->voice[i].updatePitchAdsrParameters();
}

float CoreSampler::getPitchReleaseDurationSeconds(void)
{
    return data->pitchEnvelopeParameters.getReleaseDurationSeconds();
}
