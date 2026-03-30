#pragma once
#include <sdrplay_api.h>
#include <vector>
#include <mutex>
#include <iostream>
#include <atomic>

#define POWER_HISTORY_SIZE 100

class SdrHandler {
public:
    SdrHandler();
    ~SdrHandler();

    bool InitializeAPI();
    bool StartStream(double startFreqHz);
    bool TuneFrequency(double newFreqHz);
    void ShutdownSDR();
    float GetCurrentPower() {
        std::lock_guard<std::mutex> lock(powerMutex);
        return powerHistory.empty() ? -100.0f : powerHistory.back();
    }
    void ClearPowerHistory() {
        std::lock_guard<std::mutex> lock(powerMutex);
        powerHistory.clear();
    }

    sdrplay_api_DeviceT* getDeviceHandle() { return &chosenDevice; }
    std::atomic<bool> isStreaming;

private:
    sdrplay_api_DeviceT chosenDevice;
    sdrplay_api_DeviceParamsT* deviceParams;
    
    std::vector<float> powerHistory;
    std::mutex powerMutex;
    

    static void StreamCallback(short *xi, short *xq, sdrplay_api_StreamCbParamsT *params,
                               unsigned int numSamples, unsigned int reset, void *cbContext);
    static void EventCallback(sdrplay_api_EventT eventId, sdrplay_api_TunerSelectT tuner,
                              sdrplay_api_EventParamsT *params, void *cbContext);
};