#include "sdr_handler.hpp"
#include "RdsDecoder.hpp"
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

std::string SdrHandler::GetStationName(double freqMHz) {
    // 1. Tune to the target frequency
    TuneFrequency(freqMHz * 1000000.0);
    
    // Give the hardware PLL 40ms to lock and flush the old static
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    // 2. Prepare the bucket
    // Assuming a 250,000 Hz sample rate during the narrow scan.
    // We need about 1.5 seconds of data to guarantee a full RDS looping packet.
    size_t sampleRate = 250000; 
    size_t requiredSamples = sampleRate * 1.5; 

    // 2. Prepare the bucket
    {
        std::lock_guard<std::mutex> lock(rds_mutex);
        rds_iq_buffer.clear();
        rds_iq_buffer.reserve(3000000); // Reserve a massive chunk so memory doesn't stall
        
        // Start the stopwatch and pull the trigger!
        rds_start_time = std::chrono::steady_clock::now();
        rds_capture_active = true; 
    }

    // 3. Put this thread to sleep until the callback fills the bucket
    std::unique_lock<std::mutex> wait_lock(rds_mutex);
    
    if (rds_cv.wait_for(wait_lock, std::chrono::milliseconds(4500), [this]{ return !rds_capture_active.load(); })) {
        
        // THE FIX: Calculate the exact sample rate dynamically!
        // (Total Floats / 2 for I&Q pairs) divided by 1.5 seconds
        float actualSampleRate = 500000.0f;
        
        std::cout << "[RDS] Captured 1.5s. True Sample Rate: " << actualSampleRate << " Hz" << std::endl;
        
        std::string name = RdsDecoder::ExtractName(rds_iq_buffer, actualSampleRate);
        return name;
        
    } else {
        std::cout << "[RDS] Capture timeout!" << std::endl;
        rds_capture_active = false;
        return "TIMEOUT";
    }
}

bool SdrHandler::StartStream(double startFreqHz) {
    if (!deviceParams) return false;

    // We no longer need the Forced Reset hack because ShutdownSDR is fixed!

    // 1. Update DSP Module: 2.0 MHz input, decimate by 40, 0.0 offset (Because IF is Zero)
    // Pass the target sample rate (16000) directly instead of the decimation factor
    dspModule = new IqToWav(2000000, 16000, "server/src/AudioFile/audio.wav");

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

std::string SdrHandler::GetLiveRdsText() {
    // We assume the SDR is ALREADY tuned and streaming audio!
    // 1. Prepare the bucket
    {
        std::lock_guard<std::mutex> lock(rds_mutex);
        rds_iq_buffer.clear();
        rds_iq_buffer.reserve(3500000); 
        
        rds_start_time = std::chrono::steady_clock::now();
        rds_capture_active = true; 
    }

    // 2. Sleep until the live audio callback fills the 3.5s bucket
    std::unique_lock<std::mutex> wait_lock(rds_mutex);
    
    if (rds_cv.wait_for(wait_lock, std::chrono::milliseconds(4500), [this]{ return !rds_capture_active.load(); })) {
        float actualSampleRate = 500000.0f; 
        std::cout << "[RDS] Background capture complete. Decoding..." << std::endl;
        
        return RdsDecoder::ExtractName(rds_iq_buffer, actualSampleRate);
    } else {
        rds_capture_active = false;
        return "UNKNOWN";
    }
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

    // 1. Calculate dBFS power for this callback window
    float sumMagSq = 0.0f;
    for (unsigned int i = 0; i < numSamples; ++i) {
        float i_val = static_cast<float>(xi[i]);
        float q_val = static_cast<float>(xq[i]);
        sumMagSq += (i_val * i_val) + (q_val * q_val);
    }
    float meanMagSq = sumMagSq / static_cast<float>(numSamples);
    float maxValSq  = 32767.0f * 32767.0f;
    float dbFS      = 10.0f * std::log10((meanMagSq / maxValSq) + 1e-10f);

    // 2. Push into rolling history, evict oldest if full
    {
        std::lock_guard<std::mutex> lock(sdr->powerMutex);
        sdr->powerHistory.push_back(dbFS);
        if ((int)sdr->powerHistory.size() > POWER_HISTORY_SIZE) {
            sdr->powerHistory.erase(sdr->powerHistory.begin());
        }
    }

    // ====================================================================
    // 3. THE RDS HIJACK (Divert a copy of the waves if the scanner asked for them)
    // ====================================================================
    if (sdr->rds_capture_active.load()) {
        std::lock_guard<std::mutex> lock(sdr->rds_mutex);
        
        for (unsigned int i = 0; i < numSamples; ++i) {
            sdr->rds_iq_buffer.push_back(xi[i] / 32768.0f);
            sdr->rds_iq_buffer.push_back(xq[i] / 32768.0f);
        }
        
        // Check the stopwatch
        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - sdr->rds_start_time).count();
        
        // If exactly 3.5 seconds have passed, shut it down
        if (elapsed >= 3500) {
            sdr->rds_capture_active.store(false); 
            sdr->rds_cv.notify_one();             
        }
    }
    // ====================================================================

    // 4. Feed I/Q samples to the DSP engine for demodulation + recording
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