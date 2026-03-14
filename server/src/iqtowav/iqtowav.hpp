#pragma once

#include <string>
#include <vector>
#include <cstdint>
#include <mutex>     
#include <atomic>
#include "AudioFile.h" // The new library!

class IqToWav {
public:
    IqToWav(int inputSampleRate, int outputSampleRate, std::string filename);
    ~IqToWav();

    void ProcessBlock(short *xi, short *xq, unsigned int numSamples);
    void StartRecording();
    void StopRecording();

private:
    int inputRate;
    int outputRate;
    int decimation;
    std::string outFilename;

    std::atomic<bool> isRecording; 
    std::mutex fileMutex;
    
    // THE FIX: Replaces all manual file streaming and header logic
    AudioFile<float> audioFile;

    float prev_phase = 0.0f;
    float aa_state1 = 0.0f;       
    float aa_state2 = 0.0f;
    float aa_state3 = 0.0f;
    float de_state = 0.0f;       
    float audioAccum = 0.0f;     
    int decimationCounter = 0;
    
};