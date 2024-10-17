# ``DunneAudioKit/Sampler``
@Metadata {
    @DocumentationExtension(mergeBehavior:append) 
}

**Sampler** is a polyphonic sample-playback engine built from scratch in C++.  It is 64-voice polyphonic and features a per-voice, stereo low-pass filter with resonance and ADSR envelopes for both amplitude and filter cutoff. Samples must be loaded into memory and remain resident there; it does not do streaming.  It reads standard audio files via **AVAudioFile**, as well as a more efficient Wavpack compressed format.

### Sampler vs AppleSampler

**AppleSampler** and its companion class **MIDISampler** are wrappers for Apple's *AUSampler* Audio Unit, an exceptionally powerful polyphonic, multi-timbral sampler instrument which is built-in to both macOS and iOS. Unfortunately, *AUSampler* is far from perfect and not properly documented. This **Sampler** is an attempt to provide an open-source alternative.

**Sampler** is nowhere near as powerful as *AUSampler*. If your app depends on **AppleSampler** or the **MIDISampler** wrapper class, you should continue to use it.

### Loading samples
**Sampler** provides several distinct mechanisms for loading samples:

1. ``Sampler/loadSFZ(url:)``  or ``Sampler/init(sfzURL:)`` loads entire sets of samples by interpreting a simplistic subset of the "SFZ" soundfont file format.  SFZ is a simple plain-text format which is easy to understand and edit manually. More information on <doc:Preparing-Sample-Sets>. 
2. ``Sampler/load(avAudioFile:)`` loads an ``AVAudio/AVAudioFile`` and uses it as sampler sound, assuming the file holds a middle A at 440Hz.
3. ``SamplerData`` and ``SampleDescriptor`` have many member variables to define details like the sample's natural MIDI note-number and pitch (frequency), plus details about loop start and end points, if used. See more in <doc:Sampler-descriptors>. Use ``Sampler/init()``then  ``update(data:)``. This method is the most versatile:
    - use sample data already in memory, e.g. data generated programmatically
    - read sample data using custom file-reading code
    - read compressed wavpack files with ``SamplerData/loadCompressedSampleFile(from:)``
    - mapping of MIDI-pairs (note number, velocity) to samples is done using some internal lookup tables, which can be populated in one of two ways: ``SamplerData/buildKeyMap()`` and ``SamplerData/buildSimpleKeyMap()`` 

**Important:** Before loading a new group of samples, you must call `unloadAllSamples()`. Otherwise, the new samples will be loaded *in addition* to the already-loaded ones. This wastes memory and worse, newly-loaded samples will usually not sound at all, because the sampler simply plays the first matching sample it finds.
