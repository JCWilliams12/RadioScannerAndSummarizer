#pragma once
#include <sdrplay_api.h>
#include <iostream>
#include <atomic>
#include <thread>
#include <chrono>
#include <vector>
#include <numeric>
#include <mutex>
#include "../iqtowav/iqtowav.hpp"

class SdrHandler {
public:
    SdrHandler();
    ~SdrHandler();

    bool InitializeAPI();
    bool StartStream(double startFreqHz);
    bool TuneFrequency(double newFreqHz);
    void ShutdownSDR();

    // Returns a stable averaged power reading across recent callbacks
    float GetCurrentPower() const {
        std::lock_guard<std::mutex> lock(powerMutex);
        if (powerHistory.empty()) return -100.0f;
        float sum = 0.0f;
        for (float v : powerHistory) sum += v;
        return sum / static_cast<float>(powerHistory.size());
    }

    // Call this immediately after TuneFrequency() to discard stale readings
    void ClearPowerHistory() {
        std::lock_guard<std::mutex> lock(powerMutex);
        powerHistory.clear();
    }

    sdrplay_api_DeviceT* getDeviceHandle() { return &chosenDevice; }

    IqToWav* dspModule;

private:
    sdrplay_api_DeviceParamsT* deviceParams;
    sdrplay_api_DeviceT chosenDevice;
    bool isStreaming;

    // Rolling power history - stores last N callback readings
    // N=8 at ~4ms/callback = ~32ms averaging window, stable but responsive
    mutable std::mutex powerMutex;
    std::vector<float> powerHistory;
    static constexpr int POWER_HISTORY_SIZE = 8;

    static void StreamCallback(short *xi, short *xq,
                               sdrplay_api_StreamCbParamsT *params,
                               unsigned int numSamples,
                               unsigned int reset, void *cbContext);

    static void EventCallback(sdrplay_api_EventT eventId,
                              sdrplay_api_TunerSelectT tuner,
                              sdrplay_api_EventParamsT *params,
                              void *cbContext);
};