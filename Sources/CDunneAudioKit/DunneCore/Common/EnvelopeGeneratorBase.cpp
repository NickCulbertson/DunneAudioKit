// Copyright AudioKit. All Rights Reserved.

#include "EnvelopeGeneratorBase.h"
#include <cmath>
#include <cassert>

namespace DunneCore
{

    void ExponentialSegmentGenerator::reset(double initialValue, double targetValue, double tco, int segmentLengthSamples)
    {
        output = segmentLengthSamples > 0 ? initialValue : targetValue;
        target = targetValue;
        isHorizontal = targetValue == initialValue;
        isLinear = tco <= 0.0;
        isRising = targetValue > initialValue;

        if (isHorizontal)
        {
            tcount = 0;
            segLength = segmentLengthSamples;
        }
        else if (isLinear)
        {
            if (segmentLengthSamples <= 0)
                coefficient = target - output;
            else
                coefficient = (targetValue - initialValue) / segmentLengthSamples;
        }
        else
        {
            if (segmentLengthSamples == 0)
            {
                coefficient = 0.0;
                offset = target;
            }
            else
            {
                // Correction to Pirkle (who uses delta = 1.0 always)
                // According to Redmon (who only discusses the delta = 1.0 case), delta should be defined thus
                double delta = abs(targetValue - initialValue);
                coefficient = exp(-log((delta + tco) / tco) / segmentLengthSamples);
                if (isRising)
                    offset = (target + tco) * (1.0 - coefficient);
                else
                    offset = (target - tco) * (1.0 - coefficient);
            }
        }
    }

    void MultiSegmentEnvelopeGenerator::setupCurSeg()
    {
        // Guard against calling before init() has populated the segments
        // vector (e.g. panic() walking voice slots before any preset loads,
        // or a transport-stop sweep on a fresh AU instance). Without this
        // we dereference an empty std::vector → crash. Treat as a no-op
        // since there's no envelope state to set up yet.
        if (!segments || segments->empty() ||
            curSegIndex < 0 || curSegIndex >= (int)segments->size()) {
            return;
        }
        SegmentDescriptor seg = (*segments)[curSegIndex];
        ExponentialSegmentGenerator::reset(seg.initialValue, seg.finalValue, seg.tco, seg.lengthSamples);
    }

    void MultiSegmentEnvelopeGenerator::setupCurSeg(double initValue)
    {
        // Same guard as above — see comment in setupCurSeg().
        if (!segments || segments->empty() ||
            curSegIndex < 0 || curSegIndex >= (int)segments->size()) {
            return;
        }
        SegmentDescriptor seg = (*segments)[curSegIndex];
        double targetValue = seg.finalValue;
        bool isHorizontal = seg.initialValue == seg.finalValue;
        if (isHorizontal) { // if flat (hold) then use same value, prevents fades from currentVal to hold val
            targetValue = initValue;
        }
        ExponentialSegmentGenerator::reset(initValue, targetValue, seg.tco, seg.lengthSamples);
    }

    void MultiSegmentEnvelopeGenerator::reset(Descriptor* pDesc, int initialSegmentIndex)
    {
        segments = pDesc;
        curSegIndex = initialSegmentIndex;
        // setupCurSeg() guards against an empty/uninitialized descriptor
        // vector internally — safe to call unconditionally.
        setupCurSeg();
    }

    void MultiSegmentEnvelopeGenerator::startAtSegment(int segIndex) //puts the envelope in a 'fresh' state, allows sudden jumps to first segment
    {
        curSegIndex = segIndex;
        if (skipEmptySegments()) {
            SegmentDescriptor& seg = (*segments)[curSegIndex];
            setupCurSeg(seg.initialValue); // we are restarting, not advancing, so  always start from the first value we get to
        };
    }

    void MultiSegmentEnvelopeGenerator::advanceToSegment(int segIndex) //advances w/ awareness of state, so as to not make sudden jumps
    {
        curSegIndex = segIndex;
        if (skipEmptySegments()) {
            setupCurSeg(output); //we are advancing, not restarting, so always start from the value we are currently at
        };
    }

    bool MultiSegmentEnvelopeGenerator::skipEmptySegments() //skips over any segment w/ length 0, so as to not influence the state of the envelope
    {
        // Release-mode guard: assert(segments) is compiled out in release
        // builds, so a null descriptor here would crash on segments->size().
        // Bail safely instead.
        if (!segments) return false;

        // skip any segments that are 0-length
        while(curSegIndex < segments->size()
              && (*segments)[curSegIndex].lengthSamples == 0) {
            curSegIndex++;
        }

        // if at end of the envelope, reset
        if(curSegIndex == segments->size()) {
            reset(segments);
            return false;
        }

        return true;

    }

}
