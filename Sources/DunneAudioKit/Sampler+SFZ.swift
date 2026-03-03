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
        loadSFZ(url: url, completion: nil)
    }
    
    /// Load an SFZ at the given location with completion callback
    ///
    /// Parameters:
    ///   - url: File url to the SFZ file
    ///   - completion: Optional completion handler called when loading finishes
    ///
    public func loadSFZ(url: URL, completion: (() -> Void)?) {
        // Global-level defaults (persist across all groups and regions)
        var globalLowNote: MIDINoteNumber = 0
        var globalHighNote: MIDINoteNumber = 127
        var globalNoteNumber: MIDINoteNumber = 60
        var globalLowVel: MIDIVelocity = 0
        var globalHighVel: MIDIVelocity = 127
        var globalPan: Float32 = 0
        var globalGain: Float32 = 0
        var globalTune: Int32 = 0
        var globalLoopMode = "no_loop"
        var globalLoopStart: Float32 = 0
        var globalLoopEnd: Float32 = 0

        // Group-level defaults (persist across regions, inherit from global)
        var groupLowNote: MIDINoteNumber = 0
        var groupHighNote: MIDINoteNumber = 127
        var groupNoteNumber: MIDINoteNumber = 60
        var groupLowVel: MIDIVelocity = 0
        var groupHighVel: MIDIVelocity = 127
        var groupPan: Float32 = 0
        var groupGain: Float32 = 0
        var groupTune: Int32 = 0
        var groupLoopMode = "no_loop"
        var groupLoopStart: Float32 = 0
        var groupLoopEnd: Float32 = 0

        let samplesBaseURL = url.deletingLastPathComponent()

        // Helper to parse opcodes from accumulated line
        func parseOpcodes(_ line: String) -> [String: String] {
            var opcodes: [String: String] = [:]

            // Special handling for sample parameter (may contain spaces)
            if let sampleRange = line.range(of: "sample=") {
                let sampleStart = line.index(sampleRange.upperBound, offsetBy: 0)
                var sampleEnd = line.endIndex

                // Find end of sample value (next whitespace followed by key=)
                var searchIndex = sampleStart
                while searchIndex < line.endIndex {
                    if line[searchIndex].isWhitespace {
                        let remaining = line[searchIndex...].trimmingCharacters(in: .whitespaces)
                        if remaining.contains("=") {
                            let nextPart = remaining.components(separatedBy: .whitespaces).first ?? ""
                            if nextPart.contains("=") && !nextPart.hasPrefix("=") {
                                sampleEnd = searchIndex
                                break
                            }
                        }
                    }
                    searchIndex = line.index(after: searchIndex)
                }

                let sampleValue = String(line[sampleStart..<sampleEnd]).trimmingCharacters(in: .whitespaces)
                opcodes["sample"] = sampleValue
            }

            // Parse other opcodes normally
            for part in line.components(separatedBy: .whitespaces) where !part.isEmpty && !part.hasPrefix("sample=") {
                let keyValue = part.components(separatedBy: "=")
                if keyValue.count == 2 {
                    let key = keyValue[0]
                    let value = keyValue[1]
                    if key != "sample" {  // Skip sample, already handled
                        opcodes[key] = value
                    }
                }
            }

            return opcodes
        }

        // Helper to process global section
        func processGlobal(_ line: String) {
            let opcodes = parseOpcodes(line)

            // Update global defaults
            if let key = opcodes["key"], let num = MIDINoteNumber(key) {
                globalNoteNumber = num
                globalLowNote = num
                globalHighNote = num
            }
            if let lokey = opcodes["lokey"], let num = MIDINoteNumber(lokey) {
                globalLowNote = num
            }
            if let hikey = opcodes["hikey"], let num = MIDINoteNumber(hikey) {
                globalHighNote = num
            }
            if let center = opcodes["pitch_keycenter"], let num = MIDINoteNumber(center) {
                globalNoteNumber = num
            }
            if let lovel = opcodes["lovel"], let vel = MIDIVelocity(lovel) {
                globalLowVel = vel
            }
            if let hivel = opcodes["hivel"], let vel = MIDIVelocity(hivel) {
                globalHighVel = vel
            }
            if let tune = opcodes["tune"] {
                globalTune = Int32(tune) ?? 0
            }
            if let volume = opcodes["volume"] {
                globalGain = Float32(volume) ?? 0.0
            }
            if let pan = opcodes["pan"] {
                let rawPan = Float32(pan) ?? 0.0
                globalPan = max(-100.0, min(100.0, rawPan)) / 100.0
            }
            if let loopMode = opcodes["loop_mode"] {
                globalLoopMode = loopMode
            }
            if let loopStart = opcodes["loop_start"] {
                globalLoopStart = Float32(loopStart) ?? 0
            }
            if let loopEnd = opcodes["loop_end"] {
                globalLoopEnd = Float32(loopEnd) ?? 0
            }

            // Also update group defaults so regions without explicit <group> inherit from global
            groupLowNote = globalLowNote
            groupHighNote = globalHighNote
            groupNoteNumber = globalNoteNumber
            groupLowVel = globalLowVel
            groupHighVel = globalHighVel
            groupPan = globalPan
            groupGain = globalGain
            groupTune = globalTune
            groupLoopMode = globalLoopMode
            groupLoopStart = globalLoopStart
            groupLoopEnd = globalLoopEnd
        }

        // Helper to process group section
        func processGroup(_ line: String) {
            let opcodes = parseOpcodes(line)

            // Reset group defaults to inherit from global
            groupLowNote = globalLowNote
            groupHighNote = globalHighNote
            groupNoteNumber = globalNoteNumber
            groupLowVel = globalLowVel
            groupHighVel = globalHighVel
            groupPan = globalPan
            groupGain = globalGain
            groupTune = globalTune
            groupLoopMode = globalLoopMode
            groupLoopStart = globalLoopStart
            groupLoopEnd = globalLoopEnd

            // Override with group-specific values
            if let key = opcodes["key"], let num = MIDINoteNumber(key) {
                groupNoteNumber = num
                groupLowNote = num
                groupHighNote = num
            }
            if let lokey = opcodes["lokey"], let num = MIDINoteNumber(lokey) {
                groupLowNote = num
            }
            if let hikey = opcodes["hikey"], let num = MIDINoteNumber(hikey) {
                groupHighNote = num
            }
            if let center = opcodes["pitch_keycenter"], let num = MIDINoteNumber(center) {
                groupNoteNumber = num
            }
            if let lovel = opcodes["lovel"], let vel = MIDIVelocity(lovel) {
                groupLowVel = vel
            }
            if let hivel = opcodes["hivel"], let vel = MIDIVelocity(hivel) {
                groupHighVel = vel
            }
            if let tune = opcodes["tune"] {
                groupTune = Int32(tune) ?? 0
            }
            if let volume = opcodes["volume"] {
                groupGain = Float32(volume) ?? 0.0
            }
            if let pan = opcodes["pan"] {
                let rawPan = Float32(pan) ?? 0.0
                groupPan = max(-100.0, min(100.0, rawPan)) / 100.0
            }
            if let loopMode = opcodes["loop_mode"] {
                groupLoopMode = loopMode
            }
            if let loopStart = opcodes["loop_start"] {
                groupLoopStart = Float32(loopStart) ?? 0
            }
            if let loopEnd = opcodes["loop_end"] {
                groupLoopEnd = Float32(loopEnd) ?? 0
            }
        }

        // Helper to process region section
        func processRegion(_ line: String) {
            let opcodes = parseOpcodes(line)

            // Start with group defaults
            var lowNoteNumber = groupLowNote
            var highNoteNumber = groupHighNote
            var noteNumber = groupNoteNumber
            var lowVelocity = groupLowVel
            var highVelocity = groupHighVel
            var regionPan: Float32 = 0
            var regionGain: Float32 = 0
            var regionTune: Int32 = 0
            var loopMode = groupLoopMode
            var loopStartPoint = groupLoopStart
            var loopEndPoint = groupLoopEnd
            var startPoint: Float32 = 0
            var endPoint: Float32 = 0

            // Override with region-specific values
            if let key = opcodes["key"], let num = MIDINoteNumber(key) {
                noteNumber = num
                lowNoteNumber = num
                highNoteNumber = num
            }
            if let lokey = opcodes["lokey"], let num = MIDINoteNumber(lokey) {
                lowNoteNumber = num
            }
            if let hikey = opcodes["hikey"], let num = MIDINoteNumber(hikey) {
                highNoteNumber = num
            }
            if let center = opcodes["pitch_keycenter"], let num = MIDINoteNumber(center) {
                noteNumber = num
            }
            if let lovel = opcodes["lovel"], let vel = MIDIVelocity(lovel) {
                lowVelocity = vel
            }
            if let hivel = opcodes["hivel"], let vel = MIDIVelocity(hivel) {
                highVelocity = vel
            }
            if let tune = opcodes["tune"] {
                regionTune = Int32(tune) ?? 0
            }
            if let volume = opcodes["volume"] {
                regionGain = Float32(volume) ?? 0.0
            }
            if let pan = opcodes["pan"] {
                let rawPan = Float32(pan) ?? 0.0
                regionPan = max(-100.0, min(100.0, rawPan)) / 100.0
            }
            if let mode = opcodes["loop_mode"] {
                loopMode = mode
            }
            if let loopStart = opcodes["loop_start"] {
                loopStartPoint = Float32(loopStart) ?? 0
            }
            if let loopEnd = opcodes["loop_end"] {
                loopEndPoint = Float32(loopEnd) ?? 0
            }
            if let start = opcodes["start"] {
                startPoint = Float32(start) ?? 0
            }
            if let end = opcodes["end"] {
                endPoint = Float32(end) ?? 0
            }

            // Sample is required
            guard let sample = opcodes["sample"], !sample.isEmpty else {
                Log("Warning: Region without sample defined. Skipping.")
                return
            }

            // Calculate totals
            let totalPan = groupPan + regionPan
            let totalGain = groupGain + regionGain
            let totalTune = groupTune + regionTune

            let noteFrequency = Float(440.0 * pow(2.0, (Double(noteNumber) - 69.0) / 12.0))

            Log("load \(noteNumber) \(noteFrequency) NN range \(lowNoteNumber)-\(highNoteNumber) vel \(lowVelocity)-\(highVelocity) \(sample)")

            // Create sample descriptor
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

            // Load the sample file
            let normalizedSample = sample.replacingOccurrences(of: "\\", with: "/")
            let sampleFileURL = samplesBaseURL.appendingPathComponent(normalizedSample)

            if normalizedSample.hasSuffix(".wv") {
                sampleFileURL.path.withCString { path in
                    loadCompressedSampleFile(from: SampleFileDescriptor(
                        sampleDescriptor: sampleDescriptor,
                        path: path
                    ))
                }
            } else if normalizedSample.hasSuffix(".aif") || normalizedSample.hasSuffix(".wav") {
                // Check for compressed version first
                let compressedFileURL = samplesBaseURL
                    .appendingPathComponent(String(normalizedSample.dropLast(4) + ".wv"))

                if FileManager.default.fileExists(atPath: compressedFileURL.path) {
                    compressedFileURL.path.withCString { path in
                        loadCompressedSampleFile(from: SampleFileDescriptor(
                            sampleDescriptor: sampleDescriptor,
                            path: path
                        ))
                    }
                } else {
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

        do {
            let data = try String(contentsOf: url, encoding: .utf8)
            let lines = data.components(separatedBy: .newlines)

            var currentSection = ""
            var accumulatedLine = ""

            for line in lines {
                let trimmed = String(line.trimmingCharacters(in: .whitespacesAndNewlines))

                // Skip empty lines and comments
                if trimmed.isEmpty || trimmed.hasPrefix("//") {
                    continue
                }

                // Check for section headers
                if trimmed.hasPrefix("<global>") {
                    // Process accumulated section
                    if currentSection == "region" && !accumulatedLine.isEmpty {
                        processRegion(accumulatedLine)
                    } else if currentSection == "group" && !accumulatedLine.isEmpty {
                        processGroup(accumulatedLine)
                    } else if currentSection == "global" && !accumulatedLine.isEmpty {
                        processGlobal(accumulatedLine)
                    }
                    currentSection = "global"
                    accumulatedLine = String(trimmed.dropFirst(8))
                    continue
                }
                else if trimmed.hasPrefix("<group>") {
                    // Process accumulated section
                    if currentSection == "region" && !accumulatedLine.isEmpty {
                        processRegion(accumulatedLine)
                    } else if currentSection == "group" && !accumulatedLine.isEmpty {
                        processGroup(accumulatedLine)
                    } else if currentSection == "global" && !accumulatedLine.isEmpty {
                        processGlobal(accumulatedLine)
                    }
                    currentSection = "group"
                    accumulatedLine = String(trimmed.dropFirst(7))
                    continue
                }
                else if trimmed.hasPrefix("<region>") {
                    // Process accumulated section
                    if currentSection == "region" && !accumulatedLine.isEmpty {
                        processRegion(accumulatedLine)
                    } else if currentSection == "group" && !accumulatedLine.isEmpty {
                        processGroup(accumulatedLine)
                    } else if currentSection == "global" && !accumulatedLine.isEmpty {
                        processGlobal(accumulatedLine)
                    }
                    currentSection = "region"
                    accumulatedLine = String(trimmed.dropFirst(8))
                    continue
                }

                // Accumulate opcodes (lines that don't start with <)
                if !currentSection.isEmpty && !trimmed.hasPrefix("<") {
                    if !accumulatedLine.isEmpty {
                        accumulatedLine += " "
                    }
                    accumulatedLine += trimmed
                }
            }

            // Process final accumulated section
            if currentSection == "region" && !accumulatedLine.isEmpty {
                processRegion(accumulatedLine)
            } else if currentSection == "group" && !accumulatedLine.isEmpty {
                processGroup(accumulatedLine)
            } else if currentSection == "global" && !accumulatedLine.isEmpty {
                processGlobal(accumulatedLine)
            }

        } catch {
            Log("Unable to load sound pack. The file may be corrupted or incomplete.")
        }

        buildKeyMap()
        completion?()
    }
}
