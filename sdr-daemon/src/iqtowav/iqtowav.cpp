#define _USE_MATH_DEFINES
#include <cmath>
#include <iostream>
#include <thread>
#include "iqtowav.hpp"

extern void broadcastAudio(const std::vector<int16_t>& audioBuffer);

IqToWav::IqToWav(int inputSampleRate, int outputSampleRate, std::string filename)
    : inputRate(inputSampleRate), outputRate(outputSampleRate),
      outFilename(filename), isRecording(false) {
    decimation = inputRate / outputRate;
    
    // Initialize AudioFile Settings
    audioFile.setNumChannels(1);             // Mono
    audioFile.setSampleRate(outputRate);     // 16000 Hz
    audioFile.setBitDepth(16);               // Standard PCM for Whisper
}

IqToWav::~IqToWav() {
    StopRecording();
}

void IqToWav::StartRecording() {
    std::lock_guard<std::mutex> lock(fileMutex);
    
    // Prepare the AudioFile RAM buffer
    audioFile.samples.resize(1);
    audioFile.samples[0].clear();
    // Pre-allocate for 35 seconds to prevent memory fragmentation
    audioFile.samples[0].reserve(outputRate * 35); 
    
    isRecording = true; 
    std::cout << "AudioFile RAM recording started..." << std::endl;
}

void IqToWav::StopRecording() {
    isRecording = false; 
    
    // Let the hardware finish its last callback
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    
    std::lock_guard<std::mutex> lock(fileMutex); 
    if (!audioFile.samples[0].empty()) {
        // AudioFile handles ALL header math and bit-depth formatting automatically!
        audioFile.save(outFilename);
        std::cout << "Finished recording. File saved securely by AudioFile." << std::endl;
        audioFile.samples[0].clear();
    }
}

void IqToWav::ProcessBlock(short *xi, short *xq, unsigned int numSamples) {
    std::vector<int16_t> liveAudioBuffer;
    liveAudioBuffer.reserve((numSamples / decimation) + 1);

    const float aa_alpha = 0.05f;    
    const float de_alpha = 0.4545f;  
    const float FM_GAIN = 4.25f; 
    
    // Low-Pass Filter constant for a 200kHz channel at 2.0 MSPS
    const float iq_alpha = 0.2f; 

    for (unsigned int i = 0; i < numSamples; i++) {
        float bb_i = static_cast<float>(xi[i]);
        float bb_q = static_cast<float>(xq[i]);

        // THE FIX: Digital Channel Filter
        // Strips away the 1.3 MHz of adjacent static BEFORE the FM math
        i_state = (iq_alpha * bb_i) + ((1.0f - iq_alpha) * i_state);
        q_state = (iq_alpha * bb_q) + ((1.0f - iq_alpha) * q_state);

        // Calculate phase using the FILTERED radio waves, not the raw ones
        float current_phase = std::atan2(q_state, i_state);
        float phase_diff = current_phase - prev_phase;

        if (phase_diff >  M_PI) phase_diff -= 2.0f * M_PI;
        if (phase_diff < -M_PI) phase_diff += 2.0f * M_PI;
        prev_phase = current_phase;

        aa_state1 = (aa_alpha * phase_diff) + ((1.0f - aa_alpha) * aa_state1);
        aa_state2 = (aa_alpha * aa_state1)  + ((1.0f - aa_alpha) * aa_state2);
        aa_state3 = (aa_alpha * aa_state2)  + ((1.0f - aa_alpha) * aa_state3);

        audioAccum += aa_state3;
        decimationCounter++;

        if (decimationCounter >= decimation) {
            float raw_audio = (audioAccum / static_cast<float>(decimation)) * FM_GAIN;

            de_state = (de_alpha * raw_audio) + ((1.0f - de_alpha) * de_state);
            
            float pristine_float = de_state; 

            // THE FIX: Hard-clamp the audio so it can NEVER distort or wrap around
            if (pristine_float > 1.0f) pristine_float = 1.0f;
            if (pristine_float < -1.0f) pristine_float = -1.0f;

            // PATH 1: Send perfect float data to AudioFile
            if (isRecording) {
                std::lock_guard<std::mutex> lock(fileMutex);
                audioFile.samples[0].push_back(pristine_float);
            }

            // PATH 2: Convert to 16-bit integer for the Live WebSockets
            float live_int = pristine_float * 32767.0f;

            if (live_int >  32767.0f) live_int =  32767.0f;
            if (live_int < -32768.0f) live_int = -32768.0f;
            
            liveAudioBuffer.push_back(static_cast<int16_t>(live_int));

            audioAccum = 0.0f;
            decimationCounter = 0;
        }
    }

    if (!liveAudioBuffer.empty()) {
        std::thread([liveAudioBuffer]() {
            broadcastAudio(liveAudioBuffer);
        }).detach();
    }
}