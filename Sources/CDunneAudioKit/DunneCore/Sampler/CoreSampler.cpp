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
, isFilterEnabled(true)
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

// Find a voice playing a specific note
DunneCore::SamplerVoice* CoreSampler::findVoice(unsigned noteNumber) {
    for (int i = 0; i < MAX_POLYPHONY; i++) {
        if (data->voice[i].noteNumber == noteNumber && data->voice[i].sampleBuffer != nullptr) {
            return &data->voice[i];
        }
    }
    return nullptr;
}

// Find any active voice
DunneCore::SamplerVoice* CoreSampler::findActiveVoice() {
    for (int i = 0; i < MAX_POLYPHONY; i++) {
        if (data->voice[i].noteNumber >= 0 && data->voice[i].sampleBuffer != nullptr) {
            return &data->voice[i];
        }
    }
    return nullptr;
}

// Find a free voice
DunneCore::SamplerVoice* CoreSampler::findFreeVoice() {
    for (int i = 0; i < MAX_POLYPHONY; i++) {
        if (data->voice[i].noteNumber < 0) {
            return &data->voice[i];
        }
    }
    return nullptr;
}

// Update tracking for a voice
void CoreSampler::updateActiveNoteTracking(uint32_t instanceID, unsigned noteNumber, bool isInRelease) {
    for (auto& entry : activeNotes) {
        if (std::get<1>(entry) == instanceID) {
            std::get<0>(entry) = noteNumber;
            std::get<2>(entry) = isInRelease;
            return;
        }
    }
    // If not found, add new entry
    activeNotes.push_back({noteNumber, instanceID, isInRelease});
}

// Remove a voice from activeNotes
void CoreSampler::removeFromActiveNotes(uint32_t instanceID) {
    for (auto it = activeNotes.begin(); it != activeNotes.end();) {
        if (std::get<1>(*it) == instanceID) {
            it = activeNotes.erase(it);
        } else {
            ++it;
        }
    }
}

// Set mono/legato modes
void CoreSampler::setMode(bool mono, bool legato) {
    if (mono != isMonophonic || legato != isLegato) {
        isMonophonic = mono;
        isLegato = legato;
    }
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
    
    // Reset tune value but apply it to the frequency
    pBuf->noteNumber = sdd.sampleDescriptor.noteNumber;
    pBuf->tune = 0;
    pBuf->noteFrequency = sdd.sampleDescriptor.noteFrequency * powf(2.0f, -sdd.sampleDescriptor.tune / 1200.0f);
    
    // Handle rare case where loopEndPoint is 0 (uninitialized)
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

// Map every MIDI note to closest sample buffer by pitch
void CoreSampler::buildSimpleKeyMap()
{
    // Clear old mapping
    isKeyMapValid = false;
    for (int i=0; i < MIDI_NOTENUMBERS; i++)
    {
        data->keyMap[i].clear();
    }

    for (int nn=0; nn < MIDI_NOTENUMBERS; nn++)
    {
        float noteFreq = data->tuningTable[nn];
        
        // Find minimum distance to note nn
        float minDistance = 1000000.0f;
        for (DunneCore::KeyMappedSampleBuffer *pBuf : data->sampleBufferList)
        {
            float distance = fabsf(NOTE_HZ(pBuf->noteNumber) - noteFreq);
            if (distance < minDistance)
            {
                minDistance = distance;
            }
        }
        
        // Add only samples at this distance
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

// Rebuild keyMap based on explicit mapping data in samples
void CoreSampler::buildKeyMap(void)
{
    // Clear old mapping
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

void CoreSampler::resetLFOStart()
{
    data->vibratoLFO.resetSync(); // Reset vibrato LFO to start phase
    data->globalLFO.resetSync();  // Reset global LFO to start phase
}

void CoreSampler::addHeldNote(unsigned noteNumber)
{
    heldNotes.erase(std::remove(heldNotes.begin(), heldNotes.end(), noteNumber), heldNotes.end());
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

// Play a note
void CoreSampler::playNote(unsigned noteNumber, unsigned velocity)
{
    if (stoppingAllVoices) return;
    
    // Get sample buffers and register key
    auto buffers = lookupSamples(noteNumber, velocity);
    if (buffers.empty()) return;
    data->pedalLogic.keyDownAction(noteNumber);
    
    // Update held notes
    removeHeldNote(noteNumber);
    addHeldNote(noteNumber);
    bool anotherKeyWasDown = heldNotes.size() > 1;
    
    if (isMonophonic) {
        // Mono mode: find active voice to update
        DunneCore::SamplerVoice* pActiveVoice = findActiveVoice();
        
        if (pActiveVoice && anotherKeyWasDown) {
            // Update existing voice
            if (isLegato) {
                pActiveVoice->restartNewNoteLegato(noteNumber, currentSampleRate, data->tuningTable[noteNumber]);
            } else {
                pActiveVoice->restartNewNoteMono(noteNumber, currentSampleRate, data->tuningTable[noteNumber]);
            }
            updateActiveNoteTracking(pActiveVoice->instanceID, noteNumber, false);
        } else {
            // Start fresh
            play(noteNumber, velocity, anotherKeyWasDown);
        }
    } else {
        // Polyphonic mode
        play(noteNumber, velocity, anotherKeyWasDown);
    }
}

// Stop a note
void CoreSampler::stopNote(unsigned noteNumber, bool immediate)
{
    if (stoppingAllVoices) return;
    
    // Get current state
    bool wasHeld = std::find(heldNotes.begin(), heldNotes.end(), noteNumber) != heldNotes.end();
    DunneCore::SamplerVoice* playingVoice = findVoice(noteNumber);
    bool isPlayingNote = (playingVoice != nullptr);
    
    // Remove from held notes and get next note
    removeHeldNote(noteNumber);
    unsigned newLastHeldNote = getLastHeldNote();
    
    // Handle monophonic mode
    if (isMonophonic && wasHeld && !immediate && isPlayingNote) {
        if (newLastHeldNote != (unsigned)-1 && newLastHeldNote != noteNumber) {
            // Transition to next note
            float nextNoteFrequency = data->tuningTable[newLastHeldNote];
            
            if (isLegato) {
                playingVoice->restartNewNoteLegato(newLastHeldNote, currentSampleRate, nextNoteFrequency);
            } else {
                playingVoice->restartNewNoteMono(newLastHeldNote, currentSampleRate, nextNoteFrequency);
            }
            
            updateActiveNoteTracking(playingVoice->instanceID, newLastHeldNote, false);
            playingVoice->noteNumber = newLastHeldNote;
            playingVoice->noteFrequency = nextNoteFrequency;
            return;
        } else if (newLastHeldNote == (unsigned)-1) {
            // No more notes, release the voice
            playingVoice->release(loopThruRelease);
            updateActiveNoteTracking(playingVoice->instanceID, noteNumber, true);
            return;
        }
        return;
    }
    
    // Handle polyphonic mode
    if (immediate || data->pedalLogic.keyUpAction(noteNumber)) {
        for (int i = 0; i < MAX_POLYPHONY; i++) {
            DunneCore::SamplerVoice* pVoice = &data->voice[i];
            if (pVoice->noteNumber == noteNumber) {
                if (immediate) {
                    pVoice->stop();
                    removeFromActiveNotes(pVoice->instanceID);
                } else {
                    pVoice->release(loopThruRelease);
                    updateActiveNoteTracking(pVoice->instanceID, noteNumber, true);
                }
            }
        }
    }
}

// Start a voice playing a note
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

        // First try to find an appropriate voice
        if (isMonophonic) {
            pVoice = findActiveVoice();
        }
        if (!pVoice) {
            pVoice = findFreeVoice();
        }

        // Voice stealing if needed (polyphonic only)
        if (!pVoice && !isMonophonic) {
            // Try to steal a voice in release phase
            for (auto it = activeNotes.begin(); it != activeNotes.end(); ++it) {
                if (std::get<2>(*it)) { // In release phase
                    uint32_t instanceID = std::get<1>(*it);
                    for (int i = 0; i < MAX_POLYPHONY; i++) {
                        if (data->voice[i].instanceID == instanceID) {
                            pVoice = &data->voice[i];
                            activeNotes.erase(it);
                            break;
                        }
                    }
                    if (pVoice) break;
                }
            }

            // If needed, steal oldest note
            if (!pVoice && !activeNotes.empty()) {
                auto& oldestNote = activeNotes.front();
                uint32_t instanceID = std::get<1>(oldestNote);

                for (int i = 0; i < MAX_POLYPHONY; i++) {
                    if (data->voice[i].instanceID == instanceID) {
                        pVoice = &data->voice[i];
                        break;
                    }
                }
                pVoice->stop();
                activeNotes.erase(activeNotes.begin());
            }
        }

        // Start the voice
        if (pVoice) {
            pVoice->start(noteNumber, currentSampleRate, detunedFrequency, velocity / 127.0f, pBuf);
            pVoice->setGain(pBuf->volume);
            pVoice->setPan(pBuf->pan);
            
            lastPlayedNoteNumber = noteNumber;
            activeNotes.push_back({noteNumber, pVoice->instanceID, false});
        }
    }
}

void CoreSampler::stopAllVoicesMonophonic() {
    // Use the same thread-safe approach as stopAllVoices
    stoppingAllVoices = true;
    activeNotes.clear();
    
    // Wait until Render() has killed all active notes
    bool noteStillSounding = true;
    while (noteStillSounding)
    {
        noteStillSounding = false;
        for (int i=0; i < MAX_POLYPHONY; i++)
            if (data->voice[i].noteNumber >= 0) noteStillSounding = true;
    }
    
    stoppingAllVoices = false;
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
            if (data->pedalLogic.isNoteSustaining(nn))
            {
                for (int i = 0; i < MAX_POLYPHONY; i++)
                {
                    DunneCore::SamplerVoice &voice = data->voice[i];
                    if (voice.noteNumber == nn && !voice.isInRelease)
                    {
                        stop(nn, false);
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
    heldNotes.clear();
    activeNotes.clear();
    
    // Wait until Render() has killed all active notes
    bool noteStillSounding = true;
    while (noteStillSounding)
    {
        noteStillSounding = false;
        for (int i=0; i < MAX_POLYPHONY; i++)
            if (data->voice[i].noteNumber >= 0) noteStillSounding = true;
    }
}

void CoreSampler::restartVoices()
{
    stoppingAllVoices = false;
}

void CoreSampler::render(unsigned channelCount, unsigned sampleCount, float *outBuffers[])
{
    float *pOutLeft = outBuffers[0];
    float *pOutRight = outBuffers[1];

    // Clear output buffers with simple loop - often more efficient in real-world audio code
    for (unsigned i = 0; i < sampleCount; i++)
    {
        pOutLeft[i] = 0.0f;
        pOutRight[i] = 0.0f;
    }
    
    // Use the same conditional as original
    float cutoffMul = isFilterEnabled ? cutoffMultiple : -1.0f;

    // Update LFOs exactly as before
    data->globalLFO.setFrequency(lfoRate);
    float globalLFOValue = data->globalLFO.getSample() * lfoDepth;
    data->vibratoLFO.setFrequency(vibratoFrequency);
    float pitchDev = this->pitchOffset + vibratoDepth * data->vibratoLFO.getSample();

    // Process each voice - keep original loop structure which may be better optimized by compiler
    for (int i = 0; i < MAX_POLYPHONY; i++)
    {
        DunneCore::SamplerVoice *pVoice = &data->voice[i];
        if (pVoice->noteNumber >= 0)
        {
            bool shouldStop = pVoice->prepToGetSamples(sampleCount, masterVolume, pitchDev, cutoffMul,
                                                    keyTracking, cutoffEnvelopeStrength, filterEnvelopeVelocityScaling,
                                                    linearResonance, pitchADSRSemitones, voiceVibratoDepth, voiceVibratoFrequency,
                                                    globalLFOValue, lfoTargetPitchToggle, lfoTargetGainToggle, lfoTargetFilterToggle);
            if (shouldStop)
            {
                // Stay with original logic
                removeFromActiveNotes(pVoice->instanceID);
                pVoice->stop();
            }
            else
            {
                pVoice->getSamples(sampleCount, pOutLeft, pOutRight);
            }
        }
    }
    
    // Precompute gain conversion once
    float overallGainLinear = powf(10.0f, overallGain / 20.0f);
    float leftPan = (overallPan <= 0.0f) ? 1.0f : (1.0f - overallPan);
    float rightPan = (overallPan >= 0.0f) ? 1.0f : (1.0f + overallPan);
    
    // Apply master gain and pan - this remains unchanged
    for (unsigned i = 0; i < sampleCount; i++)
    {
        float leftValue = pOutLeft[i] * overallGainLinear * leftPan;
        float rightValue = pOutRight[i] * overallGainLinear * rightPan;
        pOutLeft[i] = leftValue;
        pOutRight[i] = rightValue;
    }
}

// Legacy method used internally
void CoreSampler::stop(unsigned noteNumber, bool immediate)
{
    for (int i = 0; i < MAX_POLYPHONY; i++)
    {
        DunneCore::SamplerVoice* pVoice = &data->voice[i];
        if (pVoice->noteNumber == noteNumber)
        {
            if (immediate)
            {
                pVoice->stop();
                removeFromActiveNotes(pVoice->instanceID);
            }
            else
            {
                pVoice->release(loopThruRelease);
                updateActiveNoteTracking(pVoice->instanceID, noteNumber, true);
            }
        }
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
