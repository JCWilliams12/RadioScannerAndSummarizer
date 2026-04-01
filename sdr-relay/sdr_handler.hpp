#pragma once
// =============================================================================
// sdr-relay/sdr_handler.hpp — SDRplay hardware abstraction
// =============================================================================
// This is the ONLY file in the project that includes sdrplay_api.h.
// It exposes a clean C++ interface that the relay's main.cpp drives.
// =============================================================================

#include <atomic>
#include <sdrplay_api.h>

class SdrHandler {
public:
    SdrHandler();
    ~SdrHandler();

    // Lifecycle
    bool  InitializeAPI();
    bool  StartStream(double initialFreqHz);
    void  ShutdownSDR();

    // Tuning
    void  TuneFrequency(double hz);

    // Power measurement (lock-free, safe to call from any thread)
    float GetCurrentPower();
    void  ClearPowerHistory();

    // Direct access to the device handle — needed by setBandwidth() and
    // gain/AGC configuration in the relay's command dispatcher.
    sdrplay_api_DeviceT* getDeviceHandle() { return &chosenDevice; }

private:
    sdrplay_api_DeviceT       chosenDevice{};
    sdrplay_api_DeviceParamsT* deviceParams;
    std::atomic<bool>         isStreaming;

    // SDRplay API callbacks (static — context pointer recovers `this`)
    static void StreamACallback(short *xi, short *xq,
                                sdrplay_api_StreamCbParamsT *params,
                                unsigned int numSamples,
                                unsigned int reset, void *cbContext);

    static void EventCallback(sdrplay_api_EventT eventId,
                              sdrplay_api_TunerSelectT tuner,
                              sdrplay_api_EventParamsT *params,
                              void *cbContext);
};