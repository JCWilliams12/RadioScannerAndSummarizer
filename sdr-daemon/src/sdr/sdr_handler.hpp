#ifndef SDR_HANDLER_HPP
#define SDR_HANDLER_HPP

#include <sdrplay_api.h>
#include <vector>
#include <mutex>
#include <atomic>

#define POWER_HISTORY_SIZE 50

class SdrHandler {
public:
    SdrHandler();
    ~SdrHandler();

    bool InitializeAPI();
    bool StartStream(double initialFreq);
    void ShutdownSDR();
    
    // Updated to match the new void return type in the .cpp
    void TuneFrequency(double hz);

    // Declared here, implemented in the .cpp (fixes the redefinition error)
    float GetCurrentPower();
    void ClearPowerHistory();

    sdrplay_api_DeviceT* getDeviceHandle() { return &chosenDevice; }

private:
    sdrplay_api_DeviceT chosenDevice;
    sdrplay_api_DeviceParamsT *deviceParams;
    std::atomic<bool> isStreaming;

    std::vector<float> powerHistory;
    std::mutex powerMutex;

    // Updated to exactly match the API's required static callback signatures
    static void StreamACallback(short *xi, short *xq, sdrplay_api_StreamCbParamsT *params,
                                unsigned int numSamples, unsigned int reset, void *cbContext);
    
    static void EventCallback(sdrplay_api_EventT eventId, sdrplay_api_TunerSelectT tuner,
                              sdrplay_api_EventParamsT *params, void *cbContext);
};

#endif // SDR_HANDLER_HPP