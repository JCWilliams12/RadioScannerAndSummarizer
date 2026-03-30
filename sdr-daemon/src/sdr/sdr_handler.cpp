#include "sdr_handler.hpp"
#include <cmath>
#include <vector>
#include <mutex>
#include <iostream>

extern void broadcastAudio(const std::vector<int16_t>& audioBuffer);

SdrHandler::SdrHandler() : deviceParams(nullptr), isStreaming(false) {}

SdrHandler::~SdrHandler() {
    ShutdownSDR();
}

bool SdrHandler::InitializeAPI() {
    sdrplay_api_ErrT err;
    if ((err = sdrplay_api_Open()) != sdrplay_api_Success) return false;

    sdrplay_api_LockDeviceApi();

    sdrplay_api_DeviceT devs[SDRPLAY_MAX_DEVICES];
    unsigned int numDevs = 0;
    sdrplay_api_GetDevices(devs, &numDevs, SDRPLAY_MAX_DEVICES);

    if (numDevs == 0) {
        sdrplay_api_UnlockDeviceApi();
        sdrplay_api_Close();
        return false;
    }

    bool deviceFound = false;
    for (unsigned int i = 0; i < numDevs; i++) {
        if (devs[i].hwVer == SDRPLAY_RSPdx_ID || devs[i].hwVer == 7) {
            chosenDevice = devs[i];
            deviceFound = true;
            break;
        }
    }

    if (!deviceFound) {
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

    deviceParams->devParams->fsFreq.fsHz = 2000000.0;
    deviceParams->devParams->rspDxParams.antennaSel = sdrplay_api_RspDx_ANTENNA_C;

    sdrplay_api_RxChannelParamsT* chParams = deviceParams->rxChannelA;
    chParams->tunerParams.rfFreq.rfHz = startFreqHz;
    chParams->tunerParams.bwType = sdrplay_api_BW_1_536;
    chParams->tunerParams.ifType = sdrplay_api_IF_Zero;

    chParams->ctrlParams.dcOffset.DCenable = 1;
    chParams->ctrlParams.dcOffset.IQenable = 1;

    chParams->ctrlParams.decimation.enable = 1;
    chParams->ctrlParams.decimation.decimationFactor = 8;
    chParams->ctrlParams.decimation.wideBandSignal = 0;

    sdrplay_api_CallbackFnsT cbFns;
    cbFns.StreamACbFn = SdrHandler::StreamCallback;
    cbFns.StreamBCbFn = nullptr;
    cbFns.EventCbFn   = SdrHandler::EventCallback;

    sdrplay_api_ErrT err = sdrplay_api_Init(chosenDevice.dev, &cbFns, this);
    if (err != sdrplay_api_Success) return false;

    isStreaming = true;
    return true;
}

bool SdrHandler::TuneFrequency(double newFreqHz) {
    if (!isStreaming) return false;
    deviceParams->rxChannelA->tunerParams.rfFreq.rfHz = newFreqHz;
    sdrplay_api_ErrT err = sdrplay_api_Update(
        chosenDevice.dev, chosenDevice.tuner,
        sdrplay_api_Update_Tuner_Frf, sdrplay_api_Update_Ext1_None
    );
    return (err == sdrplay_api_Success);
}

void SdrHandler::ShutdownSDR() {
    if (isStreaming) {
        sdrplay_api_Uninit(chosenDevice.dev);
        isStreaming = false;
    }
    if (deviceParams != nullptr) {
        sdrplay_api_ReleaseDevice(&chosenDevice);
        sdrplay_api_Close();
        deviceParams = nullptr;
    }
}

void SdrHandler::StreamCallback(short *xi, short *xq, sdrplay_api_StreamCbParamsT *params,
                                unsigned int numSamples, unsigned int reset, void *cbContext) {
    SdrHandler* sdr = static_cast<SdrHandler*>(cbContext);
    if (!sdr || reset) return;

    float sumMagSq = 0.0f;
    for (unsigned int i = 0; i < numSamples; ++i) {
        float i_val = static_cast<float>(xi[i]);
        float q_val = static_cast<float>(xq[i]);
        sumMagSq += (i_val * i_val) + (q_val * q_val);
    }
    float dbFS = 10.0f * std::log10((sumMagSq / static_cast<float>(numSamples)) / (32767.0f * 32767.0f) + 1e-10f);

    {
        std::lock_guard<std::mutex> lock(sdr->powerMutex);
        sdr->powerHistory.push_back(dbFS);
        if ((int)sdr->powerHistory.size() > POWER_HISTORY_SIZE) {
            sdr->powerHistory.erase(sdr->powerHistory.begin());
        }
    }

    if (sdr->isStreaming.load()) {
        std::vector<int16_t> iqBuffer(numSamples * 2);
        for (unsigned int i = 0; i < numSamples; ++i) {
            iqBuffer[2 * i]     = xi[i];
            iqBuffer[2 * i + 1] = xq[i];
        }
        broadcastAudio(iqBuffer);
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