// Copyright AudioKit. All Rights Reserved.

// This file is safe to include in either (Objective-)C or C++ contexts.

#pragma once

typedef struct
{
    int noteNumber;
    int noteDetune;
    float noteFrequency;
    
    int minimumNoteNumber, maximumNoteNumber;
    int minimumVelocity, maximumVelocity;
    
    bool isLooping;
    float loopStartPoint, loopEndPoint;
    float startPoint, endPoint;
    float gain;
    float pan;

} SampleDescriptor;

typedef struct
{
    SampleDescriptor sampleDescriptor;
    
    float sampleRate;
    bool isInterleaved;
    int channelCount;
    int sampleCount;
    float *data;

} SampleDataDescriptor;

typedef struct
{
    SampleDescriptor sampleDescriptor;
    
    const char *path;
    
} SampleFileDescriptor;
