#pragma once
// =============================================================================
// sdr-relay/relay_config.hpp — Compile-time relay configuration
// =============================================================================

#include <cstddef>

namespace relay {

    // ---- Network ----
    static constexpr int    DATA_PORT        = 7373;
    static constexpr int    CTRL_PORT        = 7374;
    static constexpr int    LISTEN_BACKLOG   = 4;

    // ---- SDR defaults ----
    static constexpr double DEFAULT_FREQ_HZ  = 88000000.0;  // 88.0 MHz
    static constexpr double SAMPLE_RATE_HZ   = 2000000.0;   // 2 MHz master clock
    static constexpr int    DECIMATION_FACTOR = 8;          // -> 125 kHz delivered

    // ---- Ring buffer for USB→TCP bridging ----
    static constexpr int    POOL_SIZE        = 64;
    static constexpr size_t SLOT_SAMPLES     = 16384;        // IQ pairs per slot

    // ---- TCP send buffer ----
    static constexpr int    TCP_SNDBUF_SIZE  = 2 * 1024 * 1024;
    static constexpr int    TCP_RCVBUF_SIZE  = 2 * 1024 * 1024;

    // ---- Scanner ----
    static constexpr double SCAN_START_MHZ   = 88.1;
    static constexpr double SCAN_END_MHZ     = 107.9;
    static constexpr double SCAN_STEP_MHZ    = 0.1;
    static constexpr float  SCAN_SQUELCH_DB  = 12.0f;

} // namespace relay