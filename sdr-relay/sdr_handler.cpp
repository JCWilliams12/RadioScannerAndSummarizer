// =============================================================================
// sdr-relay/sdr_handler.cpp — SDRplay USB hardware driver
// =============================================================================
// This file is structurally identical to the original sdr-daemon version.
// The only difference is WHERE it lives and WHO provides broadcastAudio():
//   - Original:  sdr-daemon container → broadcastAudio() feeds DSP ring buffer
//   - Now:       sdr-relay native app → broadcastAudio() feeds TCP ring buffer
//
// The extern linkage to broadcastAudio() is resolved by the relay's main.cpp.
// =============================================================================

#include "sdr_handler.hpp"
#include <cmath>
#include <vector>
#include <mutex>
#include <iostream>
#include <atomic>
#include <chrono>

// Links to the relay's main.cpp IQ bridge
extern void broadcastAudio(const std::vector<int16_t>& audioBuffer);
extern std::atomic<bool> g_stream_gap;

// Device mode — shared with relay's scanning/configure logic
enum class DeviceMode { IDLE, SCANNING, RECORDING, LIVE_LISTEN };
extern std::atomic<DeviceMode> g_mode;

static thread_local std::vector<int16_t> t_iq_buffer;

// Lock-free atomic power tracking (used by scanner)
std::atomic<float> g_fast_power(-100.0f);

// Direct hardware callback rate measurement
static std::atomic<int>  g_cb_sample_count(0);
static std::atomic<bool> g_cb_first_call(true);
static std::chrono::steady_clock::time_point g_cb_wall_start;


SdrHandler::SdrHandler() : deviceParams(nullptr), isStreaming(false) {
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

    if (!sdr || !sdr->isStreaming.load(std::memory_order_relaxed)) return;

    if (reset) {
        g_stream_gap.store(true, std::memory_order_release);
    }

    // Hardware callback rate measurement (runs on EVERY callback)
    if (g_cb_first_call.exchange(false, std::memory_order_relaxed)) {
        g_cb_wall_start = std::chrono::steady_clock::now();
        g_cb_sample_count.store(0, std::memory_order_relaxed);
    }
    int prev = g_cb_sample_count.fetch_add(numSamples, std::memory_order_relaxed);
    int total = prev + (int)numSamples;
    if (total >= 250000) {
        auto now = std::chrono::steady_clock::now();
        float elapsed = std::chrono::duration<float>(now - g_cb_wall_start).count();
        float hw_rate = total / elapsed;
        std::cout << "[SDR-CB] *** HARDWARE RATE: " << total
                  << " samples in " << elapsed << "s = "
                  << hw_rate << " samp/sec"
                  << "  |  numSamples/callback=" << numSamples
                  << std::endl;
        g_cb_sample_count.store(0, std::memory_order_relaxed);
        g_cb_wall_start = now;
    }

    // Lock-Free Power Calculation (always runs — scanner needs it)
    float sumMagSq = 0.0f;
    for (unsigned int i = 0; i < numSamples; ++i) {
        float i_val = static_cast<float>(xi[i]);
        float q_val = static_cast<float>(xq[i]);
        sumMagSq += (i_val * i_val) + (q_val * q_val);
    }
    float dbFS = 10.0f * std::log10((sumMagSq / static_cast<float>(numSamples))
                                    / (32767.0f * 32767.0f) + 1e-10f);
    g_fast_power.store(dbFS, std::memory_order_relaxed);

    // Interleave I/Q and hand off to the relay's TCP bridge
    t_iq_buffer.resize(numSamples * 2);
    for (unsigned int i = 0; i < numSamples; ++i) {
        t_iq_buffer[2 * i]     = xi[i];
        t_iq_buffer[2 * i + 1] = xq[i];
    }

    broadcastAudio(t_iq_buffer);
}


void SdrHandler::EventCallback(sdrplay_api_EventT eventId, sdrplay_api_TunerSelectT tuner,
                               sdrplay_api_EventParamsT *params, void *cbContext) {
    SdrHandler* sdr = static_cast<SdrHandler*>(cbContext);
    if (!sdr) return;

    if (eventId == sdrplay_api_PowerOverloadChange) {
        if (params->powerOverloadParams.powerOverloadChangeType == sdrplay_api_Overload_Detected) {
            if (g_mode.load(std::memory_order_relaxed) == DeviceMode::SCANNING) {
                sdrplay_api_Update(sdr->chosenDevice.dev, sdr->chosenDevice.tuner,
                    (sdrplay_api_ReasonForUpdateT)sdrplay_api_Update_Ctrl_OverloadMsgAck,
                    sdrplay_api_Update_Ext1_None);
            }
        }
    }
}


bool SdrHandler::StartStream(double initialFreq) {
    sdrplay_api_ErrT err;
    if ((err = sdrplay_api_SelectDevice(&chosenDevice)) != sdrplay_api_Success) return false;

    sdrplay_api_GetDeviceParams(chosenDevice.dev, &deviceParams);

    deviceParams->devParams->fsFreq.fsHz = 2000000.0;
    deviceParams->rxChannelA->tunerParams.rfFreq.rfHz = initialFreq;
    deviceParams->rxChannelA->tunerParams.bwType = sdrplay_api_BW_0_600;
    deviceParams->rxChannelA->tunerParams.ifType = sdrplay_api_IF_Zero;

    deviceParams->rxChannelA->tunerParams.gain.LNAstate = 4;
    deviceParams->rxChannelA->ctrlParams.agc.enable = sdrplay_api_AGC_50HZ;
    deviceParams->rxChannelA->ctrlParams.agc.setPoint_dBfs = -30;

    deviceParams->rxChannelA->ctrlParams.dcOffset.DCenable = 1;
    deviceParams->rxChannelA->ctrlParams.dcOffset.IQenable = 1;

    deviceParams->rxChannelA->ctrlParams.decimation.enable          = 1;
    deviceParams->rxChannelA->ctrlParams.decimation.decimationFactor = 4;
    deviceParams->rxChannelA->ctrlParams.decimation.wideBandSignal   = 0;

    std::cout << "\n[SDR] ====== PRE-INIT CONFIG ======" << std::endl;
    std::cout << "  fsFreq.fsHz      = " << deviceParams->devParams->fsFreq.fsHz << " Hz" << std::endl;
    std::cout << "  decimation.enable = " << (int)deviceParams->rxChannelA->ctrlParams.decimation.enable << std::endl;
    std::cout << "  decimation.factor = " << (int)deviceParams->rxChannelA->ctrlParams.decimation.decimationFactor << std::endl;
    std::cout << "  Expected output   = " << (deviceParams->devParams->fsFreq.fsHz / deviceParams->rxChannelA->ctrlParams.decimation.decimationFactor) << " Hz" << std::endl;
    std::cout << "==================================\n" << std::endl;

    sdrplay_api_CallbackFnsT cbFns;
    cbFns.StreamACbFn = StreamACallback;
    cbFns.StreamBCbFn = nullptr;
    cbFns.EventCbFn   = EventCallback;

    if ((err = sdrplay_api_Init(chosenDevice.dev, &cbFns, this)) != sdrplay_api_Success) {
        std::cerr << "[SDR] Init failed: " << sdrplay_api_GetErrorString(err) << std::endl;
        return false;
    }

    // Post-init readback
    sdrplay_api_DeviceParamsT* readback = nullptr;
    sdrplay_api_GetDeviceParams(chosenDevice.dev, &readback);
    if (readback && readback->devParams && readback->rxChannelA) {
        std::cout << "\n[SDR] ====== POST-INIT READBACK ======" << std::endl;
        std::cout << "  fsFreq.fsHz      = " << readback->devParams->fsFreq.fsHz << " Hz" << std::endl;
        std::cout << "  decimation.enable = " << (int)readback->rxChannelA->ctrlParams.decimation.enable << std::endl;
        std::cout << "  decimation.factor = " << (int)readback->rxChannelA->ctrlParams.decimation.decimationFactor << std::endl;
        std::cout << "  Expected output   = " << (readback->devParams->fsFreq.fsHz / readback->rxChannelA->ctrlParams.decimation.decimationFactor) << " Hz" << std::endl;
        std::cout << "======================================\n" << std::endl;
    }

    isStreaming = true;
    g_cb_first_call.store(true, std::memory_order_relaxed);

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
    deviceParams->rxChannelA->tunerParams.rfFreq.rfHz = hz;
    sdrplay_api_Update(chosenDevice.dev, chosenDevice.tuner,
        sdrplay_api_Update_Tuner_Frf, sdrplay_api_Update_Ext1_None);
}

float SdrHandler::GetCurrentPower() {
    return g_fast_power.load(std::memory_order_relaxed);
}

void SdrHandler::ClearPowerHistory() {
    g_fast_power.store(-100.0f, std::memory_order_relaxed);
}