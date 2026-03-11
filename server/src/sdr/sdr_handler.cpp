#include "sdr_handler.hpp"
#include <cmath>

extern void broadcastAudio(const std::vector<int16_t>& audioBuffer);

SdrHandler::SdrHandler() : deviceParams(nullptr), isStreaming(false), dspModule(nullptr) {}

SdrHandler::~SdrHandler() {
    ShutdownSDR();
}

bool SdrHandler::InitializeAPI() {
    sdrplay_api_ErrT err;

    if ((err = sdrplay_api_Open()) != sdrplay_api_Success) {
        std::cerr << "API Open failed: " << sdrplay_api_GetErrorString(err) << std::endl;
        return false;
    }

    sdrplay_api_LockDeviceApi();

    sdrplay_api_DeviceT devs[SDRPLAY_MAX_DEVICES];
    unsigned int numDevs = 0;
    sdrplay_api_GetDevices(devs, &numDevs, SDRPLAY_MAX_DEVICES);

    if (numDevs == 0) {
        std::cerr << "No SDRplay devices found!" << std::endl;
        sdrplay_api_UnlockDeviceApi();
        sdrplay_api_Close();
        return false;
    }

    bool deviceFound = false;
    for (unsigned int i = 0; i < numDevs; i++) {
        std::cout << "Detected Device #" << i << ": ID=" << (int)devs[i].hwVer 
                  << " SN=" << devs[i].SerNo << std::endl;
        
        // ID 4 = RSPdx, ID 7 = RSPdx-R
        if (devs[i].hwVer == SDRPLAY_RSPdx_ID || devs[i].hwVer == 7) { 
            chosenDevice = devs[i];
            deviceFound = true;
            std::cout << "RSPdx compatible hardware locked in!" << std::endl;
            break;
        }
    }

    if (!deviceFound) {
        std::cerr << "RSPdx not found." << std::endl;
        sdrplay_api_UnlockDeviceApi();
        sdrplay_api_Close();
        return false;
    }

    sdrplay_api_SelectDevice(&chosenDevice);
    sdrplay_api_UnlockDeviceApi();
    sdrplay_api_GetDeviceParams(chosenDevice.dev, &deviceParams);

    return true;
}

bool SdrHandler::StartStream(double startFreqHz) {
    if (!deviceParams) return false;

    // We no longer need the Forced Reset hack because ShutdownSDR is fixed!

    // 1. Update DSP Module: 2.0 MHz input, decimate by 40, 0.0 offset (Because IF is Zero)
    // Pass the target sample rate (16000) directly instead of the decimation factor
    dspModule = new IqToWav(2000000, 16000, "server/src/whispertinytest/audio.wav");

    // 2. Update Hardware Sample Rate to 2.0 MHz
    deviceParams->devParams->fsFreq.fsHz = 2000000.0;
    deviceParams->devParams->rspDxParams.antennaSel = sdrplay_api_RspDx_ANTENNA_C;

    sdrplay_api_RxChannelParamsT* chParams = deviceParams->rxChannelA;
    chParams->tunerParams.rfFreq.rfHz = startFreqHz;
    chParams->tunerParams.bwType = sdrplay_api_BW_1_536;
    chParams->tunerParams.ifType = sdrplay_api_IF_Zero;

    sdrplay_api_CallbackFnsT cbFns;
    cbFns.StreamACbFn = SdrHandler::StreamCallback; 
    cbFns.StreamBCbFn = nullptr;
    cbFns.EventCbFn   = SdrHandler::EventCallback;

    sdrplay_api_ErrT err = sdrplay_api_Init(chosenDevice.dev, &cbFns, this);
    
    if (err != sdrplay_api_Success) {
        std::cerr << "CRITICAL: SDRplay Init failed: " << sdrplay_api_GetErrorString(err) << std::endl;
        return false;
    }

    isStreaming = true;
    return true;
}

bool SdrHandler::TuneFrequency(double newFreqHz) {
    if (!isStreaming) return false;

    deviceParams->rxChannelA->tunerParams.rfFreq.rfHz = newFreqHz;

    sdrplay_api_ErrT err = sdrplay_api_Update(
        chosenDevice.dev, 
        chosenDevice.tuner, 
        sdrplay_api_Update_Tuner_Frf, 
        sdrplay_api_Update_Ext1_None
    );

    return (err == sdrplay_api_Success);
}

void SdrHandler::ShutdownSDR() {
    // 1. Stop the data stream if it is running
    if (isStreaming) {
        sdrplay_api_Uninit(chosenDevice.dev);
        isStreaming = false;
    }

    // 2. ALWAYS release the hardware lock, regardless of stream state!
    if (deviceParams != nullptr) {
        sdrplay_api_ReleaseDevice(&chosenDevice);
        sdrplay_api_Close();
        deviceParams = nullptr; // Prevent double-free
    }
    
    // 3. Clean up the DSP Engine
    if (dspModule) {
        delete dspModule;
        dspModule = nullptr;
    }
}

void SdrHandler::StreamCallback(short *xi, short *xq, sdrplay_api_StreamCbParamsT *params,
                                unsigned int numSamples, unsigned int reset, void *cbContext) {
    
    SdrHandler* sdr = static_cast<SdrHandler*>(cbContext);
    if (!sdr) return;

    if (reset) return;

    // Calculate dBFS power for this callback window
    float sumMagSq = 0.0f;
    for (unsigned int i = 0; i < numSamples; ++i) {
        float i_val = static_cast<float>(xi[i]);
        float q_val = static_cast<float>(xq[i]);
        sumMagSq += (i_val * i_val) + (q_val * q_val);
    }
    float meanMagSq = sumMagSq / static_cast<float>(numSamples);
    float maxValSq  = 32767.0f * 32767.0f;
    float dbFS      = 10.0f * std::log10((meanMagSq / maxValSq) + 1e-10f);

    // Push into rolling history, evict oldest if full
    {
        std::lock_guard<std::mutex> lock(sdr->powerMutex);
        sdr->powerHistory.push_back(dbFS);
        if ((int)sdr->powerHistory.size() > POWER_HISTORY_SIZE) {
            sdr->powerHistory.erase(sdr->powerHistory.begin());
        }
    }

    // Feed I/Q samples to the DSP engine for demodulation + recording
    if (sdr->dspModule && sdr->isStreaming) {
        sdr->dspModule->ProcessBlock(xi, xq, numSamples);
    }
}

void SdrHandler::EventCallback(sdrplay_api_EventT eventId, sdrplay_api_TunerSelectT tuner,
                               sdrplay_api_EventParamsT *params, void *cbContext) {
    if (eventId == sdrplay_api_PowerOverloadChange) {
        SdrHandler* handler = static_cast<SdrHandler*>(cbContext);
        sdrplay_api_Update(handler->chosenDevice.dev, tuner,
                           sdrplay_api_Update_Ctrl_OverloadMsgAck,
                           sdrplay_api_Update_Ext1_None);
    }
}