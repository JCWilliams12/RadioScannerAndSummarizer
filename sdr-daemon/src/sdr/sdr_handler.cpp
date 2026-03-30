#include "sdr_handler.hpp"
#include <cmath>
#include <vector>
#include <mutex>
#include <iostream>

// The link to the Lock-Free Ring Buffer in main.cpp
extern void broadcastAudio(const std::vector<int16_t>& audioBuffer);

// Pre-allocated buffer to prevent heap-allocation inside the high-speed USB callback!
static thread_local std::vector<int16_t> t_iq_buffer;

SdrHandler::SdrHandler() : deviceParams(nullptr), isStreaming(false) {
    // Pre-allocate enough space for the largest possible USB callback
    t_iq_buffer.reserve(32768); 
}

SdrHandler::~SdrHandler() {
    ShutdownSDR();
}

bool SdrHandler::InitializeAPI() {
    sdrplay_api_ErrT err;
    if ((err = sdrplay_api_Open()) != sdrplay_api_Success) {
        std::cerr << "[SDR] API Open failed: " << sdrplay_api_GetErrorString(err) << std::endl;
        return false;
    }

    sdrplay_api_LockDeviceApi();

    sdrplay_api_DeviceT devs[SDRPLAY_MAX_DEVICES];
    unsigned int numDevs = 0;
    sdrplay_api_GetDevices(devs, &numDevs, SDRPLAY_MAX_DEVICES);

    if (numDevs == 0) {
        std::cerr << "[SDR] No SDRplay devices found!" << std::endl;
        sdrplay_api_UnlockDeviceApi();
        sdrplay_api_Close();
        return false;
    }

    bool deviceFound = false;
    for (unsigned int i = 0; i < numDevs; i++) {
        if (devs[i].hwVer == SDRPLAY_RSPdx_ID || devs[i].hwVer == 7) {
            chosenDevice = devs[i];
            deviceFound = true;
            std::cout << "[SDR] Bound to RSPdx (SN: " << chosenDevice.SerNo << ")" << std::endl;
            break;
        }
    }

    if (!deviceFound) {
        chosenDevice = devs[0];
        std::cout << "[SDR] RSPdx not found. Defaulting to Device 0." << std::endl;
    }

    sdrplay_api_UnlockDeviceApi();
    return true;
}

void SdrHandler::StreamACallback(short *xi, short *xq, sdrplay_api_StreamCbParamsT *params, 
                                 unsigned int numSamples, unsigned int reset, void *cbContext) {
    SdrHandler* sdr = static_cast<SdrHandler*>(cbContext);
    if (!sdr || reset) return;

    // 1. Calculate Signal Power (For the Scanner Thread)
    float sumMagSq = 0.0f;
    for (unsigned int i = 0; i < numSamples; ++i) {
        float i_val = static_cast<float>(xi[i]);
        float q_val = static_cast<float>(xq[i]);
        sumMagSq += (i_val * i_val) + (q_val * q_val);
    }
    float dbFS = 10.0f * std::log10((sumMagSq / static_cast<float>(numSamples)) / (32767.0f * 32767.0f) + 1e-10f);

    {
        // Mutex only locks the tiny power array, never the actual audio data
        std::lock_guard<std::mutex> lock(sdr->powerMutex);
        sdr->powerHistory.push_back(dbFS);
        if ((int)sdr->powerHistory.size() > POWER_HISTORY_SIZE) {
            sdr->powerHistory.erase(sdr->powerHistory.begin());
        }
    }

    // 2. The Dongle Thread Hand-off
    if (sdr->isStreaming.load()) {
        t_iq_buffer.resize(numSamples * 2);
        
        // Fast interleave
        for (unsigned int i = 0; i < numSamples; ++i) {
            t_iq_buffer[2 * i]     = xi[i];
            t_iq_buffer[2 * i + 1] = xq[i];
        }

        // Fire directly into the lock-free ring buffer in main.cpp!
        broadcastAudio(t_iq_buffer);
    }
}

void SdrHandler::EventCallback(sdrplay_api_EventT eventId, sdrplay_api_TunerSelectT tuner, 
                               sdrplay_api_EventParamsT *params, void *cbContext) {
    if (eventId == sdrplay_api_PowerOverloadChange) {
        if (params->powerOverloadParams.powerOverloadChangeType == sdrplay_api_Overload_Detected) {
            std::cerr << "[HW EVENT] OVERLOAD DETECTED! Signal too hot. ADC is clipping." << std::endl;
        } else if (params->powerOverloadParams.powerOverloadChangeType == sdrplay_api_Overload_Corrected) {
            std::cerr << "[HW EVENT] Overload Corrected (AGC applied attenuation)." << std::endl;
        } else {
            std::cerr << "[HW EVENT] Overload state changed." << std::endl;
        }
    }
}

bool SdrHandler::StartStream(double initialFreq) {
    sdrplay_api_ErrT err;
    if ((err = sdrplay_api_SelectDevice(&chosenDevice)) != sdrplay_api_Success) return false;

    sdrplay_api_GetDeviceParams(chosenDevice.dev, &deviceParams);

    // Initial Safe Settings
    deviceParams->devParams->fsFreq.fsHz = 2000000.0;
    deviceParams->rxChannelA->tunerParams.rfFreq.rfHz = initialFreq;
    deviceParams->rxChannelA->tunerParams.bwType = sdrplay_api_BW_1_536;
    deviceParams->rxChannelA->tunerParams.ifType = sdrplay_api_IF_Zero;
    
    deviceParams->rxChannelA->tunerParams.gain.LNAstate = 4;
    deviceParams->rxChannelA->ctrlParams.agc.enable = sdrplay_api_AGC_50HZ;
    deviceParams->rxChannelA->ctrlParams.agc.setPoint_dBfs = -30;

    // Remove DC Spikes internally
    deviceParams->rxChannelA->ctrlParams.dcOffset.DCenable = 1;
    deviceParams->rxChannelA->ctrlParams.dcOffset.IQenable = 1;

    // =======================================================
    // HARDWARE DECIMATION LOCK
    // This physically restricts the USB bus to 250,000 Hz, 
    // saving our Docker CPU from melting.
    // =======================================================
    deviceParams->rxChannelA->ctrlParams.decimation.enable = 0;
    deviceParams->rxChannelA->ctrlParams.decimation.decimationFactor = 1;
    deviceParams->rxChannelA->ctrlParams.decimation.wideBandSignal = 0;

    sdrplay_api_CallbackFnsT cbFns;
    cbFns.StreamACbFn = StreamACallback;
    cbFns.StreamBCbFn = nullptr;
    cbFns.EventCbFn = EventCallback;

    if ((err = sdrplay_api_Init(chosenDevice.dev, &cbFns, this)) != sdrplay_api_Success) {
        std::cerr << "[SDR] Init failed: " << sdrplay_api_GetErrorString(err) << std::endl;
        return false;
    }

    isStreaming = true;
    return true;
}

void SdrHandler::ShutdownSDR() {
    if (isStreaming) {
        sdrplay_api_Uninit(chosenDevice.dev);
        isStreaming = false;
    }
    sdrplay_api_ReleaseDevice(&chosenDevice);
}

void SdrHandler::TuneFrequency(double hz) {
    if (!deviceParams) return;
    sdrplay_api_Update(chosenDevice.dev, chosenDevice.tuner,
        sdrplay_api_Update_Tuner_Frf, sdrplay_api_Update_Ext1_None);
    deviceParams->rxChannelA->tunerParams.rfFreq.rfHz = hz;
}

float SdrHandler::GetCurrentPower() {
    std::lock_guard<std::mutex> lock(powerMutex);
    if (powerHistory.empty()) return -100.0f;
    return powerHistory.back();
}

void SdrHandler::ClearPowerHistory() {
    std::lock_guard<std::mutex> lock(powerMutex);
    powerHistory.clear();
}