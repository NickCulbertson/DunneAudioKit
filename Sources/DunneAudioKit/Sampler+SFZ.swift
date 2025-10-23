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
        var startPoint: Float32 = 0
        var endPoint: Float32 = 0
        var groupPan: Float32 = 0
        var groupGain: Float32 = 0
        var groupTune: Int32 = 0
        var regionPan: Float32 = 0
        var regionGain: Float32 = 0
        var regionTune: Int32 = 0

        let samplesBaseURL = url.deletingLastPathComponent()

        do {
            let data = try String(contentsOf: url, encoding: .ascii)
            let lines = data.components(separatedBy: .newlines)
            
            for line in lines {
                let trimmed = String(line.trimmingCharacters(in: .whitespacesAndNewlines))
                
                // Skip empty lines and comments
                if trimmed.isEmpty || trimmed.hasPrefix("//") {
                    continue
                }
                
                if trimmed.hasPrefix("<group>") {
                    // Reset group parameters
                    groupTune = 0
                    groupGain = 0.0
                    groupPan = 0.0
                    
                    // Parse group parameters
                    for part in trimmed.dropFirst(7).components(separatedBy: .whitespaces) where !part.isEmpty {
                        let keyValue = part.components(separatedBy: "=")
                        if keyValue.count != 2 { continue }
                        
                        let key = keyValue[0]
                        let value = keyValue[1]
                        
                        switch key {
                        case "key":
                            if let num = MIDINoteNumber(value) {
                                noteNumber = num
                                lowNoteNumber = num
                                highNoteNumber = num
                            }
                        case "lokey":
                            lowNoteNumber = MIDINoteNumber(value) ?? lowNoteNumber
                        case "hikey":
                            highNoteNumber = MIDINoteNumber(value) ?? highNoteNumber
                        case "pitch_keycenter":
                            noteNumber = MIDINoteNumber(value) ?? noteNumber
                        case "tune":
                            groupTune = Int32(value) ?? 0
                        case "volume":
                            groupGain = Float32(value) ?? 0.0
                        case "pan":
                            // Clamp pan to valid range
                            let rawPan = Float32(value) ?? 0.0
                            groupPan = max(-100.0, min(100.0, rawPan)) / 100.0  // Normalize to -1.0...1.0
                        default:
                            break
                        }
                    }
                }
                
                if trimmed.hasPrefix("<region>") {
                    // Reset region parameters
                    regionTune = 0
                    regionGain = 0.0
                    regionPan = 0.0
                    sample = ""
                    
                    // Default to values from last group
                    loopMode = "no_loop"
                    loopStartPoint = 0
                    loopEndPoint = 0
                    startPoint = 0
                    endPoint = 0
                    
                    // Parse region parameters
                    var regionParams = [String: String]()
                    
                    for part in trimmed.dropFirst(8).components(separatedBy: .whitespaces) where !part.isEmpty {
                        let keyValue = part.components(separatedBy: "=")
                        if keyValue.count != 2 { continue }
                        
                        let key = keyValue[0]
                        let value = keyValue[1]
                        regionParams[key] = value
                    }
                    
                    // Process sample last, after all other parameters
                    for (key, value) in regionParams {
                        switch key {
                        case "lovel":
                            lowVelocity = MIDIVelocity(value) ?? 0
                        case "hivel":
                            highVelocity = MIDIVelocity(value) ?? 127
                        case "loop_mode":
                            loopMode = value
                        case "loop_start":
                            loopStartPoint = Float32(value) ?? 0
                        case "loop_end":
                            loopEndPoint = Float32(value) ?? 0
                        case "start":
                            startPoint = Float32(value) ?? 0
                        case "end":
                            endPoint = Float32(value) ?? 0
                        case "tune":
                            regionTune = Int32(value) ?? 0
                        case "volume":
                            regionGain = Float32(value) ?? 0.0
                        case "pan":
                            // Clamp pan to valid range
                            let rawPan = Float32(value) ?? 0.0
                            regionPan = max(-100.0, min(100.0, rawPan)) / 100.0  // Normalize to -1.0...1.0
                        case "sample":
                            sample = value
                        default:
                            break
                        }
                    }
                    
                    // Calculate the total pan, gain, and detune for this region
                    let totalPan = groupPan + regionPan
                    let totalGain = groupGain + regionGain
                    let totalTune = groupTune + regionTune
                    
                    // Skip if no sample defined
                    if sample.isEmpty {
                        Log("Warning: Region without sample defined. Skipping.")
                        continue
                    }
                    
                    let noteFrequency = Float(440.0 * pow(2.0, (Double(noteNumber) - 69.0) / 12.0))
                    
                    let noteLog = "load \(noteNumber) \(noteFrequency) NN range \(lowNoteNumber)-\(highNoteNumber)"
                    Log("\(noteLog) vel \(lowVelocity)-\(highVelocity) \(sample)")
                    
                    // Create the sample descriptor
                    let sampleDescriptor = SampleDescriptor(
                        noteNumber: Int32(noteNumber),
                        tune: totalTune,
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
                    
                    // Load the sample with proper path handling
                    sample = sample.replacingOccurrences(of: "\\", with: "/")
                    let sampleFileURL = samplesBaseURL.appendingPathComponent(sample)
                    
                    if sample.hasSuffix(".wv") {
                        // Load compressed WavPack file
                        sampleFileURL.path.withCString { path in
                            loadCompressedSampleFile(from: SampleFileDescriptor(
                                sampleDescriptor: sampleDescriptor,
                                path: path
                            ))
                        }
                    } else if sample.hasSuffix(".aif") || sample.hasSuffix(".wav") {
                        // Check if a compressed version exists
                        let compressedFileURL = samplesBaseURL
                            .appendingPathComponent(String(sample.dropLast(4) + ".wv"))
                        
                        let fileMgr = FileManager.default
                        if fileMgr.fileExists(atPath: compressedFileURL.path) {
                            // Use compressed version if available
                            compressedFileURL.path.withCString { path in
                                loadCompressedSampleFile(from: SampleFileDescriptor(
                                    sampleDescriptor: sampleDescriptor,
                                    path: path
                                ))
                            }
                        } else {
                            // Otherwise load the audio file directly
                            do {
                                let sampleFile = try AVAudioFile(forReading: sampleFileURL)
                                loadAudioFile(from: sampleDescriptor, file: sampleFile)
                            } catch {
                                Log("Error loading audio file: \(error.localizedDescription)")
                            }
                        }
                    } else {
                        Log("Unsupported sample format: \(sample)")
                    }
                }
            }
        } catch {
            Log("Unable to load sound pack. The file may be corrupted or incomplete.")
        }
        
        // Build the key map after all samples are loaded
        buildKeyMap()
    }
    
    /// Load an SFZ at the given location with completion callback
    ///
    /// Parameters:
    ///   - url: File url to the SFZ file
    ///   - completion: Optional completion handler called when loading finishes
    ///
    public func loadSFZ(url: URL, completion: (() -> Void)?) {
        loadSFZ(url: url)
        completion?()
    }
}
