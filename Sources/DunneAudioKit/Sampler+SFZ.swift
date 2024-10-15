// Copyright AudioKit. All Rights Reserved.

import AudioKit
import AVFoundation
import CDunneAudioKit

/// Super-naive code to read a .sfz file

extension SamplerData {
    /// Load an SFZ at the given location
    ///
    /// Parameters:
    ///   - path: Path to the file as a string
    ///   - fileName: Name of the SFZ file
    ///
    internal func loadSFZ(path: String, fileName: String) {
        loadSFZ(url: URL(fileURLWithPath: path).appendingPathComponent(fileName))
    }

    /// Load an SFZ at the given location
    ///
    /// Parameters:
    ///   - url: File url to the SFZ file
    ///
    public func loadSFZ(url: URL) {
        var lowNoteNumber: MIDINoteNumber = 0
        var highNoteNumber: MIDINoteNumber = 127
        var noteNumber: MIDINoteNumber = 60
        var lowVelocity: MIDIVelocity = 0
        var highVelocity: MIDIVelocity = 127
        var sample = ""
        var loopMode = "no_loop"
        var loopStartPoint: Float32 = 0
        var loopEndPoint: Float32 = 0
        var startPoint: Float32 = 0  // New: Start point for sample playback
        var endPoint: Float32 = 0    // New: End point for sample playback
        var groupPan: Float32 = 0.0    // Group-level pan
        var groupGain: Float32 = 0.0   // Group-level gain
        var groupTune: Int = 0         // Group-level tune
        
        var regionPan: Float32 = 0.0   // Region-level pan
        var regionGain: Float32 = 0.0  // Region-level gain
        var regionTune: Int = 0        // Region-level tune

        let samplesBaseURL = url.deletingLastPathComponent()
        
        do {
            let data = try String(contentsOf: url, encoding: .ascii)
            let lines = data.components(separatedBy: .newlines)
            for line in lines {
                let trimmed = String(line.trimmingCharacters(in: .whitespacesAndNewlines))
                if trimmed == "" || trimmed.hasPrefix("//") {
                    // ignore blank lines and comment lines
                    continue
                }
                if trimmed.hasPrefix("<group>") {
                    // parse a <group> line
                    groupTune = 0
                    groupGain = 0.0
                    groupPan = 0.0
                    
                    for part in trimmed.dropFirst(12).components(separatedBy: .whitespaces) {
                        if part.hasPrefix("key") {
                            noteNumber = MIDINoteNumber(part.components(separatedBy: "=")[1]) ?? 0
                            lowNoteNumber = noteNumber
                            highNoteNumber = noteNumber
                        } else if part.hasPrefix("lokey") {
                            lowNoteNumber = MIDINoteNumber(part.components(separatedBy: "=")[1]) ?? 0
                        } else if part.hasPrefix("hikey") {
                            highNoteNumber = MIDINoteNumber(part.components(separatedBy: "=")[1]) ?? 0
                        } else if part.hasPrefix("pitch_keycenter") {
                            noteNumber = MIDINoteNumber(part.components(separatedBy: "=")[1]) ?? 0
                        } else if part.hasPrefix("tune") {
                            groupTune = Int(part.components(separatedBy: "=")[1]) ?? 0
                        } else if part.hasPrefix("volume") {
                            groupGain = Float(part.components(separatedBy: "=")[1]) ?? 0.0
                        } else if part.hasPrefix("pan") {
                            groupPan = Float(part.components(separatedBy: "=")[1]) ?? 0.0
                        }
                    }
                }
                if trimmed.hasPrefix("<region>") {
                    // parse a <region> line
                    regionPan = 0.0   // Reset region pan for each new region
                    regionGain = 0.0  // Reset region gain for each new region
                    regionTune = 0  // Reset region tune for each new region
                    
                    for part in trimmed.dropFirst(12).components(separatedBy: .whitespaces) {
                        if part.hasPrefix("lovel") {
                            lowVelocity = MIDIVelocity(part.components(separatedBy: "=")[1]) ?? 0
                        } else if part.hasPrefix("hivel") {
                            highVelocity = MIDIVelocity(part.components(separatedBy: "=")[1]) ?? 0
                        } else if part.hasPrefix("loop_mode") {
                            loopMode = part.components(separatedBy: "=")[1]
                        } else if part.hasPrefix("loop_start") {
                            loopStartPoint = Float32(part.components(separatedBy: "=")[1]) ?? 0
                        } else if part.hasPrefix("loop_end") {
                            loopEndPoint = Float32(part.components(separatedBy: "=")[1]) ?? 0
                        } else if part.hasPrefix("start") {  // New: Parse the start point
                            startPoint = Float32(part.components(separatedBy: "=")[1]) ?? 0
                        } else if part.hasPrefix("end") {    // New: Parse the end point
                            endPoint = Float32(part.components(separatedBy: "=")[1]) ?? 0
                        } else if part.hasPrefix("tune") {  // Region-level detune
                            regionTune = Int(part.components(separatedBy: "=")[1]) ?? 0
                        } else if part.hasPrefix("volume") {    // Region-level gain
                            regionGain = Float(part.components(separatedBy: "=")[1]) ?? 0.0
                        } else if part.hasPrefix("pan") {
                            regionPan = Float(part.components(separatedBy: "=")[1]) ?? 0.0
                        }  else if part.hasPrefix("sample") {
                            sample = trimmed.components(separatedBy: "sample=")[1]
                        }
                    }

                    // Calculate the total pan, gain, and detune for this region
                    let totalPan = groupPan + regionPan
                    let totalGain = groupGain + regionGain
                    let totalTune = groupTune + regionTune
                    
                    let noteFrequency = Float(440.0 * pow(2.0, (Double(noteNumber) - 69.0) / 12.0))

                    let noteLog = "load \(noteNumber) \(noteFrequency) NN range \(lowNoteNumber)-\(highNoteNumber)"
                    Log("\(noteLog) vel \(lowVelocity)-\(highVelocity) \(sample)")

                    let sampleDescriptor = SampleDescriptor(
                        noteNumber: Int32(noteNumber),
                        tune: Int32(totalTune), // Use the detune value from the group
                        noteFrequency: noteFrequency,
                        minimumNoteNumber: Int32(lowNoteNumber),
                        maximumNoteNumber: Int32(highNoteNumber),
                        minimumVelocity: Int32(lowVelocity),
                        maximumVelocity: Int32(highVelocity),
                        isLooping: loopMode != "no_loop",
                        loopStartPoint: loopStartPoint,
                        loopEndPoint: loopEndPoint,
                        startPoint: startPoint,
                        endPoint: endPoint,
                        volume: totalGain,
                        pan: totalPan
                    )
                    
                    sample = sample.replacingOccurrences(of: "\\", with: "/")
                    let sampleFileURL = samplesBaseURL.appendingPathComponent(sample)
                    
                    // Load the file and handle compressed or uncompressed files
                    if sample.hasSuffix(".wv") {
                        sampleFileURL.path.withCString { path in
                            loadCompressedSampleFile(from: SampleFileDescriptor(sampleDescriptor: sampleDescriptor, path: path))
                        }
                    } else if sample.hasSuffix(".aif") || sample.hasSuffix(".wav") {
                        let sampleFile = try AVAudioFile(forReading: sampleFileURL)
                        loadAudioFile(from: sampleDescriptor, file: sampleFile)
                    }
                }
            }
        } catch {
            Log("Could not load SFZ: \(error.localizedDescription)")
        }
        buildKeyMap()
    }
}
