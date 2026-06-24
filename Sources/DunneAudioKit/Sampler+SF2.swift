// Copyright AudioKit. All Rights Reserved.

import AudioKit
import CDunneAudioKit
import Foundation

// Direct SF2 preset loader for SamplerData.
//
// Reads the SF2 binary in memory, resolves the full generator hierarchy for a
// single bank/program, converts the required int16 PCM slices to float32, and
// feeds them straight to CoreSampler via loadRawSampleData — no disk writes.
//
// Usage:
//   samplerData.loadSF2(url: sf2URL, bank: 0, program: 0) { /* done */ }

extension SamplerData {

    /// Load one preset from an SF2 file directly, without writing any files.
    ///
    /// - Parameters:
    ///   - url: Path to the .sf2 file.
    ///   - bank: SF2 bank number (0 for GM melodic, 128 for GM percussion).
    ///   - program: SF2 program number (0-127).
    ///   - completion: Called on the calling thread when all samples are loaded.
    public func loadSF2(url: URL, bank: Int, program: Int, completion: (() -> Void)? = nil) {
        guard let data = try? Data(contentsOf: url, options: .mappedIfSafe) else {
            Log("SF2 load failed: could not read \(url.lastPathComponent)")
            completion?()
            return
        }
        loadSF2(data: data, bank: bank, program: program, completion: completion)
    }

    /// Load one preset from an already-loaded SF2 Data blob.
    public func loadSF2(data: Data, bank: Int, program: Int, completion: (() -> Void)? = nil) {
        guard let sf2 = try? SF2Parser.parse(data) else {
            Log("SF2 load failed: parse error")
            completion?()
            return
        }

        // Find the matching preset index
        guard let presetIndex = sf2.presets.indices.first(where: {
            sf2.presets[$0].bank == UInt16(bank) && sf2.presets[$0].program == UInt16(program)
        }) else {
            Log("SF2 load: bank \(bank) program \(program) not found")
            completion?()
            return
        }

        let regions = sf2.collectRegions(forPresetIndex: presetIndex)
        guard !regions.isEmpty else {
            Log("SF2 load: no regions for bank \(bank) program \(program)")
            completion?()
            return
        }

        // Pre-scan: for each left-channel region, record which (rightIndex, lokey, hikey, lovel, hivel)
        // tuples are already covered by the stereo merge. Right-channel regions with the SAME range
        // are duplicates and should be skipped. Right-channel regions with a DIFFERENT range (e.g.
        // Halo Pad in Synth Strings 1) need to be loaded separately as stereo.
        struct RightCoverKey: Hashable { let rightIndex, lokey, hikey, lovel, hivel: Int }
        var coveredRightRanges = Set<RightCoverKey>()
        for r in regions {
            guard sf2.sampleHeaders[r.sampleIndex].sampleType == 4 else { continue }
            let rightIndex = r.sampleIndex + 1
            guard rightIndex < sf2.sampleHeaders.count,
                  sf2.sampleHeaders[rightIndex].sampleType == 2 else { continue }
            coveredRightRanges.insert(RightCoverKey(rightIndex: rightIndex,
                                                     lokey: r.lokey, hikey: r.hikey,
                                                     lovel: r.lovel, hivel: r.hivel))
        }

        var skippedIndices = Set<Int>()  // right-channel indices already merged into a stereo sample

        for region in regions {
            let sampleIndex = region.sampleIndex
            if skippedIndices.contains(sampleIndex) { continue }

            let header = sf2.sampleHeaders[sampleIndex]

            let rootKey = region.rootKeyOverride ?? Int(header.originalKey)
            let totalCents = region.coarseTune * 100 + region.fineTune + Int(header.correction)
            let noteFrequency = Float(440.0 * pow(2.0, (Double(rootKey) - 69.0) / 12.0))
            let clampedCb = min(960.0, max(0.0, region.attenuationCb))
            let volumeDb  = Float(-clampedCb * 0.04)
            let isLooping = (region.sampleModes & 3 == 1) || (region.sampleModes & 3 == 3)

            if header.sampleType == 4 {
                // Left channel — right partner is always at index+1 (sampleLink is unreliable in many SF2s)
                let rightIndex = sampleIndex + 1
                if rightIndex < sf2.sampleHeaders.count && sf2.sampleHeaders[rightIndex].sampleType == 2 {
                    let rHeader = sf2.sampleHeaders[rightIndex]

                    let lStart = Int(header.start) + Int(region.startOffset)
                    let lEnd   = Int(header.end)   + Int(region.endOffset)
                    let rStart = Int(rHeader.start)
                    let rEnd   = Int(rHeader.end)
                    guard lEnd > lStart, rEnd > rStart else { continue }

                    let frameCount = min(lEnd - lStart, rEnd - rStart)
                    guard (lStart + frameCount) * 2 <= sf2.smplBytes.count,
                          (rStart + frameCount) * 2 <= sf2.smplBytes.count else { continue }

                    // Non-interleaved: all left frames followed by all right frames
                    var floats = [Float](repeating: 0, count: frameCount * 2)
                    sf2.smplBytes.withUnsafeBytes { raw in
                        let src = raw.bindMemory(to: Int16.self)
                        for i in 0..<frameCount {
                            floats[i]              = Float(src[lStart + i]) / 32768.0
                            floats[frameCount + i] = Float(src[rStart + i]) / 32768.0
                        }
                    }

                    let loopStart = Float(max(0, Int32(header.startloop) - Int32(header.start) + region.startLoopOffset))
                    let loopEnd   = Float(max(0, Int32(header.endloop)   - Int32(header.start) + region.endLoopOffset))
                    let sd = SampleDescriptor(
                        noteNumber:        Int32(rootKey),
                        tune:              Int32(totalCents),
                        noteFrequency:     noteFrequency,
                        minimumNoteNumber: Int32(region.lokey),
                        maximumNoteNumber: Int32(region.hikey),
                        minimumVelocity:   Int32(region.lovel),
                        maximumVelocity:   Int32(region.hivel),
                        isLooping:         isLooping,
                        loopStartPoint:    loopStart,
                        loopEndPoint:      loopEnd,
                        startPoint:        Float(max(0, region.startOffset)),
                        endPoint:          Float(frameCount - 1),
                        volume:            volumeDb,
                        pan:               0.0
                    )
                    floats.withUnsafeMutableBufferPointer { buf in
                        var sdd = SampleDataDescriptor(
                            sampleDescriptor: sd,
                            sampleRate:       Float(header.sampleRate),
                            isInterleaved:    false,
                            channelCount:     2,
                            sampleCount:      Int32(frameCount),
                            data:             buf.baseAddress
                        )
                        akCoreSamplerLoadData(coreSamplerRef, &sdd)
                    }
                    skippedIndices.insert(rightIndex)
                    continue
                }
            }

            // Right-channel region: skip if its range is already covered by a left-channel stereo
            // load above. If the range differs (e.g. Halo Pad layer), load it as stereo using
            // its left partner's data so it plays correctly over its actual key range.
            if header.sampleType == 2 {
                let coverKey = RightCoverKey(rightIndex: sampleIndex,
                                             lokey: region.lokey, hikey: region.hikey,
                                             lovel: region.lovel, hivel: region.hivel)
                if coveredRightRanges.contains(coverKey) { continue }
                // Uncovered right-channel region — load as stereo with its left partner
                let leftIndex = sampleIndex - 1
                guard leftIndex >= 0, sf2.sampleHeaders[leftIndex].sampleType == 4 else { continue }
                let lHeader = sf2.sampleHeaders[leftIndex]
                let lStart = Int(lHeader.start); let lEnd = Int(lHeader.end)
                let rStart = Int(header.start);  let rEnd = Int(header.end)
                guard lEnd > lStart, rEnd > rStart else { continue }
                let frameCount = min(lEnd - lStart, rEnd - rStart)
                guard (lStart + frameCount) * 2 <= sf2.smplBytes.count,
                      (rStart + frameCount) * 2 <= sf2.smplBytes.count else { continue }
                let lRootKey = region.rootKeyOverride ?? Int(lHeader.originalKey)
                let lNoteFreq = Float(440.0 * pow(2.0, (Double(lRootKey) - 69.0) / 12.0))
                let lTotalCents = region.coarseTune * 100 + region.fineTune + Int(lHeader.correction)
                let loopStart = Float(max(0, Int32(lHeader.startloop) - Int32(lHeader.start) + region.startLoopOffset))
                let loopEnd   = Float(max(0, Int32(lHeader.endloop)   - Int32(lHeader.start) + region.endLoopOffset))
                var floats = [Float](repeating: 0, count: frameCount * 2)
                sf2.smplBytes.withUnsafeBytes { raw in
                    let src = raw.bindMemory(to: Int16.self)
                    for i in 0..<frameCount {
                        floats[i]              = Float(src[lStart + i]) / 32768.0
                        floats[frameCount + i] = Float(src[rStart + i]) / 32768.0
                    }
                }
                let sd = SampleDescriptor(
                    noteNumber: Int32(lRootKey), tune: Int32(lTotalCents),
                    noteFrequency: lNoteFreq,
                    minimumNoteNumber: Int32(region.lokey), maximumNoteNumber: Int32(region.hikey),
                    minimumVelocity: Int32(region.lovel), maximumVelocity: Int32(region.hivel),
                    isLooping: isLooping, loopStartPoint: loopStart, loopEndPoint: loopEnd,
                    startPoint: Float(max(0, region.startOffset)), endPoint: Float(frameCount - 1),
                    volume: volumeDb, pan: 0.0)
                floats.withUnsafeMutableBufferPointer { buf in
                    var sdd = SampleDataDescriptor(sampleDescriptor: sd,
                        sampleRate: Float(lHeader.sampleRate), isInterleaved: false,
                        channelCount: 2, sampleCount: Int32(frameCount), data: buf.baseAddress)
                    akCoreSamplerLoadData(coreSamplerRef, &sdd)
                }
                continue
            }

            let frameStart = Int(header.start) + Int(region.startOffset)
            let frameEnd   = Int(header.end)   + Int(region.endOffset)
            guard frameEnd > frameStart else { continue }
            let frameCount = frameEnd - frameStart
            guard frameEnd * 2 <= sf2.smplBytes.count else { continue }

            var floats = [Float](repeating: 0, count: frameCount)
            sf2.smplBytes.withUnsafeBytes { raw in
                let src = raw.bindMemory(to: Int16.self)
                for i in 0..<frameCount {
                    floats[i] = Float(src[frameStart + i]) / 32768.0
                }
            }

            let loopStart = Float(max(0, Int32(header.startloop) - Int32(header.start) + region.startLoopOffset))
            let loopEnd   = Float(max(0, Int32(header.endloop)   - Int32(header.start) + region.endLoopOffset))
            let panNorm   = Float(max(-100, min(100, region.pan / 5))) / 100.0
            let sd = SampleDescriptor(
                noteNumber:        Int32(rootKey),
                tune:              Int32(totalCents),
                noteFrequency:     noteFrequency,
                minimumNoteNumber: Int32(region.lokey),
                maximumNoteNumber: Int32(region.hikey),
                minimumVelocity:   Int32(region.lovel),
                maximumVelocity:   Int32(region.hivel),
                isLooping:         isLooping,
                loopStartPoint:    loopStart,
                loopEndPoint:      loopEnd,
                startPoint:        Float(max(0, region.startOffset)),
                endPoint:          Float(frameCount - 1),
                volume:            volumeDb,
                pan:               panNorm
            )
            floats.withUnsafeMutableBufferPointer { buf in
                var sdd = SampleDataDescriptor(
                    sampleDescriptor: sd,
                    sampleRate:       Float(header.sampleRate),
                    isInterleaved:    false,
                    channelCount:     1,
                    sampleCount:      Int32(frameCount),
                    data:             buf.baseAddress
                )
                akCoreSamplerLoadData(coreSamplerRef, &sdd)
            }
        }

        buildKeyMap()
        completion?()
    }
}


// MARK: - Minimal SF2 parser (private to this file)

private struct SF2Parser {

    struct Preset  { let name: String; let program: UInt16; let bank: UInt16; let bagIndex: UInt16 }
    struct Instr   { let bagIndex: UInt16 }
    struct Bag     { let genIndex: UInt16 }
    struct Gen     { let oper: UInt16; let amount: UInt16 }
    struct SampleHeader {
        let start: UInt32; let end: UInt32
        let startloop: UInt32; let endloop: UInt32
        let sampleRate: UInt32
        let originalKey: UInt8; let correction: Int8
        let sampleType: UInt16
    }

    struct Region {
        var sampleIndex: Int
        var lokey = 0;  var hikey = 127
        var lovel = 0;  var hivel = 127
        var rootKeyOverride: Int? = nil
        var coarseTune = 0; var fineTune = 0
        var attenuationCb: Double = 0
        var pan = 0
        var startOffset: Int32 = 0;  var endOffset: Int32 = 0
        var startLoopOffset: Int32 = 0; var endLoopOffset: Int32 = 0
        var sampleModes = 0
    }

    let presets: [Preset]
    let presetBags: [Bag]
    let presetGens: [Gen]
    let instrs: [Instr]
    let instBags: [Bag]
    let instGens: [Gen]
    let sampleHeaders: [SampleHeader]
    let smplBytes: Data

    // MARK: Parse

    static func parse(_ data: Data) throws -> SF2Parser {
        var r = ByteCursor(data)
        guard r.read4() == "RIFF" else { throw Err.bad("not RIFF") }
        _ = r.readU32()
        guard r.read4() == "sfbk" else { throw Err.bad("not sfbk") }

        var smpl = Data()
        var presets: [Preset] = []; var pBags: [Bag] = []; var pGens: [Gen] = []
        var instrs: [Instr] = [];   var iBags: [Bag] = []; var iGens: [Gen] = []
        var shdrs: [SampleHeader] = []

        while r.remaining >= 8 {
            guard r.read4() == "LIST" else { throw Err.bad("expected LIST") }
            let listSz = Int(r.readU32())
            let kind   = r.read4()
            let listEnd = r.pos + listSz - 4

            switch kind {
            case "sdta":
                while r.pos < listEnd {
                    let sub = r.read4(); let sz = Int(r.readU32())
                    if sub == "smpl" { smpl = r.readData(sz) } else { r.pos += sz }
                }
            case "pdta":
                while r.pos < listEnd {
                    let sub = r.read4(); let sz = Int(r.readU32()); let end = r.pos + sz
                    switch sub {
                    case "phdr":
                        for _ in 0..<(sz/38) {
                            let name = r.readStr(20); let prog = r.readU16(); let bank = r.readU16()
                            let bag  = r.readU16();   r.pos += 12
                            presets.append(Preset(name: name, program: prog, bank: bank, bagIndex: bag))
                        }
                    case "pbag": for _ in 0..<(sz/4) { let g=r.readU16(); r.pos+=2; pBags.append(Bag(genIndex:g)) }
                    case "pgen": for _ in 0..<(sz/4) { let o=r.readU16(); let a=r.readU16(); pGens.append(Gen(oper:o,amount:a)) }
                    case "inst": for _ in 0..<(sz/22) { r.pos+=20; let b=r.readU16(); instrs.append(Instr(bagIndex:b)) }
                    case "ibag": for _ in 0..<(sz/4)  { let g=r.readU16(); r.pos+=2; iBags.append(Bag(genIndex:g)) }
                    case "igen": for _ in 0..<(sz/4)  { let o=r.readU16(); let a=r.readU16(); iGens.append(Gen(oper:o,amount:a)) }
                    case "shdr":
                        for _ in 0..<(sz/46) {
                            r.pos += 20     // name (ignored)
                            let s=r.readU32(); let e=r.readU32(); let sl=r.readU32(); let el=r.readU32()
                            let sr=r.readU32(); let ok=r.readU8(); let co=Int8(bitPattern:r.readU8())
                            r.pos += 2      // sampleLink
                            let st=r.readU16()
                            shdrs.append(SampleHeader(start:s,end:e,startloop:sl,endloop:el,
                                                      sampleRate:sr,originalKey:ok,correction:co,sampleType:st))
                        }
                    default: break
                    }
                    r.pos = end
                }
            default:
                break
            }
            r.pos = listEnd
        }

        return SF2Parser(presets: presets, presetBags: pBags, presetGens: pGens,
                         instrs: instrs, instBags: iBags, instGens: iGens,
                         sampleHeaders: shdrs, smplBytes: smpl)
    }

    // MARK: Region collection (same hierarchy logic as SF2ToSFZConverter)

    func collectRegions(forPresetIndex pi: Int) -> [Region] {
        guard pi < presets.count else { return [] }
        let preset = presets[pi]
        let bagStart = Int(preset.bagIndex)
        let bagEnd   = pi + 1 < presets.count ? Int(presets[pi+1].bagIndex) : presetBags.count
        guard bagStart < bagEnd else { return [] }

        // Global preset zone
        var globalOverlay = PresetOverlay()
        let fb = presetBags[bagStart]; let fgStart = Int(fb.genIndex)
        let fgEnd = bagStart+1 < presetBags.count ? Int(presetBags[bagStart+1].genIndex) : presetGens.count
        let firstHasInst = (fgStart..<fgEnd).contains { presetGens[$0].oper == 41 }
        if !firstHasInst { applyPresetGens(fgStart..<fgEnd, into: &globalOverlay) }

        var regions: [Region] = []
        for pbIdx in bagStart..<bagEnd {
            let pgStart = Int(presetBags[pbIdx].genIndex)
            let pgEnd   = pbIdx+1 < presetBags.count ? Int(presetBags[pbIdx+1].genIndex) : presetGens.count
            var overlay = globalOverlay
            var instIdx: Int? = nil
            for i in pgStart..<pgEnd {
                let g = presetGens[i]
                if g.oper == 41 { instIdx = Int(g.amount) }
                else { applyPresetGen(g, into: &overlay) }
            }
            guard let ii = instIdx, ii < instrs.count else { continue }
            regions.append(contentsOf: collectInstRegions(ii, overlay: overlay))
        }
        return regions
    }

    private func collectInstRegions(_ ii: Int, overlay: PresetOverlay) -> [Region] {
        let bagStart = Int(instrs[ii].bagIndex)
        let bagEnd   = ii+1 < instrs.count ? Int(instrs[ii+1].bagIndex) : instBags.count
        guard bagStart < bagEnd else { return [] }

        var globalReg = Region(sampleIndex: -1)
        var globalSet = false
        var regions: [Region] = []

        for ibIdx in bagStart..<bagEnd {
            let igStart = Int(instBags[ibIdx].genIndex)
            let igEnd   = ibIdx+1 < instBags.count ? Int(instBags[ibIdx+1].genIndex) : instGens.count
            guard igStart < igEnd else { continue }

            var region = globalSet ? globalReg : Region(sampleIndex: -1)
            var sid: Int? = nil
            for i in igStart..<igEnd { applyInstGen(instGens[i], into: &region, sampleID: &sid) }

            if let s = sid, s < sampleHeaders.count {
                region.sampleIndex = s
                region.lokey = max(region.lokey, overlay.lokey)
                region.hikey = min(region.hikey, overlay.hikey)
                region.lovel = max(region.lovel, overlay.lovel)
                region.hivel = min(region.hivel, overlay.hivel)
                guard region.lokey <= region.hikey, region.lovel <= region.hivel else { continue }
                region.attenuationCb += overlay.attenuationCb
                region.pan           += overlay.pan
                region.coarseTune    += overlay.coarseTune
                region.fineTune      += overlay.fineTune
                regions.append(region)
            } else {
                globalReg = region; globalSet = true
            }
        }
        return regions
    }

    // MARK: Generator application

    private struct PresetOverlay {
        var lokey = 0; var hikey = 127; var lovel = 0; var hivel = 127
        var attenuationCb: Double = 0; var pan = 0; var coarseTune = 0; var fineTune = 0
    }

    private func applyPresetGens(_ range: Range<Int>, into o: inout PresetOverlay) {
        for i in range { applyPresetGen(presetGens[i], into: &o) }
    }

    private func applyPresetGen(_ g: Gen, into o: inout PresetOverlay) {
        switch g.oper {
        case 17: o.pan           = Int(Int16(bitPattern: g.amount))
        case 43: o.lokey = Int(g.amount & 0xff); o.hikey = Int((g.amount >> 8) & 0xff)
        case 44: o.lovel = Int(g.amount & 0xff); o.hivel = Int((g.amount >> 8) & 0xff)
        case 48: o.attenuationCb = Double(Int16(bitPattern: g.amount))  // initialAttenuation (gen 48)
        case 51: o.coarseTune    = Int(Int16(bitPattern: g.amount))     // coarseTune (gen 51)
        case 52: o.fineTune      = Int(Int16(bitPattern: g.amount))     // fineTune (gen 52)
        default: break
        }
    }

    private func applyInstGen(_ g: Gen, into r: inout Region, sampleID: inout Int?) {
        switch g.oper {
        case 0:  r.startOffset     = Int32(Int16(bitPattern: g.amount))
        case 1:  r.endOffset       = Int32(Int16(bitPattern: g.amount))
        case 2:  r.startLoopOffset = Int32(Int16(bitPattern: g.amount))
        case 3:  r.endLoopOffset   = Int32(Int16(bitPattern: g.amount))
        case 4:  r.startOffset    += Int32(Int16(bitPattern: g.amount)) * 32768
        case 12: r.endOffset      += Int32(Int16(bitPattern: g.amount)) * 32768  // endAddrsCoarseOffset (gen 12)
        case 45: r.startLoopOffset += Int32(Int16(bitPattern: g.amount)) * 32768 // startloopAddrsCoarseOffset (gen 45)
        case 50: r.endLoopOffset   += Int32(Int16(bitPattern: g.amount)) * 32768 // endloopAddrsCoarseOffset (gen 50)
        case 17: r.pan = Int(Int16(bitPattern: g.amount))
        case 43: r.lokey = Int(g.amount & 0xff); r.hikey = Int((g.amount >> 8) & 0xff)
        case 44: r.lovel = Int(g.amount & 0xff); r.hivel = Int((g.amount >> 8) & 0xff)
        case 48: r.attenuationCb = Double(Int16(bitPattern: g.amount))  // initialAttenuation (gen 48)
        case 51: r.coarseTune    = Int(Int16(bitPattern: g.amount))     // coarseTune (gen 51)
        case 52: r.fineTune      = Int(Int16(bitPattern: g.amount))     // fineTune (gen 52)
        case 53: sampleID        = Int(g.amount)                        // sampleID (gen 53)
        case 54: r.sampleModes   = Int(g.amount)                        // sampleModes (gen 54)
        case 58: let rk = Int(g.amount); if rk != 255 { r.rootKeyOverride = rk }
        default: break
        }
    }

    enum Err: Error { case bad(String) }
}


// MARK: - Byte cursor

private struct ByteCursor {
    let data: Data
    var pos: Int = 0
    var remaining: Int { data.count - pos }

    init(_ data: Data) { self.data = data }

    mutating func read4() -> String {
        let s = String(bytes: data[pos..<pos+4], encoding: .isoLatin1) ?? "????"
        pos += 4; return s
    }
    mutating func readU32() -> UInt32 {
        var v: UInt32 = 0
        _ = withUnsafeMutableBytes(of: &v) { data.copyBytes(to: $0, from: pos..<pos+4) }
        pos += 4; return v.littleEndian
    }
    mutating func readU16() -> UInt16 {
        var v: UInt16 = 0
        _ = withUnsafeMutableBytes(of: &v) { data.copyBytes(to: $0, from: pos..<pos+2) }
        pos += 2; return v.littleEndian
    }
    mutating func readU8() -> UInt8 { let v = data[pos]; pos += 1; return v }
    mutating func readStr(_ n: Int) -> String {
        let s = String(bytes: data[pos..<pos+n], encoding: .isoLatin1)?
            .trimmingCharacters(in: .init(charactersIn: "\0")) ?? ""
        pos += n; return s
    }
    mutating func readData(_ n: Int) -> Data {
        let d = data[pos..<pos+n]; pos += n; return d
    }
}
