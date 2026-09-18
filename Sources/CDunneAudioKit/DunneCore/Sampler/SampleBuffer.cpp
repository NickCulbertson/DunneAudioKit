// Copyright AudioKit. All Rights Reserved.

#include "SampleBuffer.h"

#include <new>

namespace DunneCore
{

    SampleBuffer::SampleBuffer()
    : samples(0)
    , channelCount(0)
    , sampleCount(0)
    , startPoint(0.0f)
    , endPoint(0.0f)
    , isLooping(false)
    , loopStartPoint(0.0f)
    , loopEndPoint(0.0f)
    {
    }
    
    SampleBuffer::~SampleBuffer()
    {
        deinit();
    }
    
    void SampleBuffer::init(float sampleRate, int channelCount, int sampleCount)
    {
        this->sampleRate = sampleRate;
        this->sampleCount = sampleCount;
        this->channelCount = channelCount;
        if (samples) delete[] samples;

        // On failure the buffer is left empty, which reads as silence.
        const long long elementCount = (long long)channelCount * (long long)sampleCount;
        samples = (channelCount >= 0 && sampleCount >= 0)
                      ? new (std::nothrow) float[(size_t)elementCount]
                      : 0;
        if (samples == 0)
        {
            this->channelCount = 0;
            this->sampleCount = 0;
            loopStartPoint = startPoint = 0.0f;
            loopEndPoint = endPoint = 0.0f;
            return;
        }

        loopStartPoint = startPoint = 0.0f;
        loopEndPoint = endPoint = (float)(sampleCount - 1);
    }
    
    void SampleBuffer::deinit()
    {
        if (samples) delete[] samples;
        samples = 0;
    }
    
    void SampleBuffer::setData(unsigned index, float data)
    {
        if ((int)index < channelCount * sampleCount)
        {
            samples[index] = data;
        }
    }
    
}
