# Sampler API Reference

## Core Methods

### Playback Control
- `playNote(noteNumber:velocity:)` - Trigger note playback with velocity
- `stopNote(noteNumber:immediate:)` - Stop note playback (immediate or with release)
- `sustainPedal(down:)` - Control sustain pedal state

### Playback Modes
- `setMode(mono:legato:)` - Configure monophonic and legato behavior
  - `mono: false` - Polyphonic mode (default)
  - `mono: true, legato: false` - Monophonic with envelope restarts
  - `mono: true, legato: true` - Monophonic with smooth transitions

### Sample Management
- `loadSampleData(_:)` - Load samples from SampleDataDescriptor
- `unloadAllSamples()` - Clear all loaded samples
- `buildKeyMap()` - Build key mapping from loaded samples

### Voice Management
- `findVoice(noteNumber:)` - Locate voice playing specific note
- `findActiveVoice()` - Find any currently active voice  
- `findFreeVoice()` - Locate available voice for new note

## SamplerVoice Methods

### Voice Control
- `start(noteNumber:sampleRate:frequency:volume:sampleBuffer:)` - Initialize voice for new note
- `restartNewNoteMono(noteNumber:sampleRate:frequency:)` - Restart voice in mono mode
- `restartNewNoteLegato(noteNumber:sampleRate:frequency:)` - Restart voice in legato mode
- `release(loopThruRelease:)` - Begin note release phase
- `stop()` - Immediately stop voice

### Audio Parameters
- `setGain(gainDB:)` - Set voice gain in decibels
- `setPan(panValue:)` - Set voice pan position (-1.0 to +1.0)

## Data Structures

### SampleDescriptor
```c
typedef struct {
    int noteNumber;
    int tune;                    // Detuning in cents
    float noteFrequency;
    int minimumNoteNumber, maximumNoteNumber;
    int minimumVelocity, maximumVelocity;
    bool isLooping;
    float loopStartPoint, loopEndPoint;
    float startPoint, endPoint;
    float volume;                // Linear gain adjustment
    float pan;                   // Stereo position (-1.0 to +1.0)
} SampleDescriptor;
```

### KeyMappedSampleBuffer
Extended SampleBuffer with additional fields:
- `tune` - Detuning in cents
- `volume` - Per-sample volume adjustment
- `pan` - Per-sample pan position

## Note Tracking

The sampler uses dual tracking systems:
- `heldNotes` - Tracks physical key presses for mono/legato behavior
- `activeNotes` - Tracks audio voices with unique instance IDs for voice management