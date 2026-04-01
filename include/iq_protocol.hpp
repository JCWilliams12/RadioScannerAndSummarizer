#pragma once
// =============================================================================
// include/iq_protocol.hpp — IQ-over-TCP wire format
// =============================================================================
// Shared between sdr-relay (native host) and sdr-daemon (Docker container).
// Defines the framing for the DATA port and the message conventions for the
// CONTROL port.
//
// DATA PORT (default 7373):
//   Relay → Daemon, continuous stream of framed IQ chunks:
//   ┌──────────────┬──────────────┬─────────────────────────────┐
//   │ magic  (4B)  │ count  (4B)  │  count × 2 × int16_t IQ    │
//   │ 0x49515043   │ network order│  interleaved I, Q, I, Q … │
//   └──────────────┴──────────────┴─────────────────────────────┘
//
// CONTROL PORT (default 7374):
//   Bidirectional newline-delimited JSON.
//
//   Daemon → Relay commands:
//     {"command":"TUNE","freq":88.1}
//     {"command":"CONFIGURE","freq":88.1,"bw":"BW_0_200","if_mode":"IF_Zero",
//      "agc":"off","lna":4}
//     {"command":"SCAN"}
//
//   Relay → Daemon events:
//     {"event":"configure_ack"}
//     {"event":"scan_complete","stations":[{"freq":88.1,"name":"FM 88.1"}, ...]}
//     {"event":"error","msg":"..."}
// =============================================================================

#include <cstdint>

namespace iq_protocol {

    // Frame header magic bytes: ASCII "IQPC"
    static constexpr uint32_t MAGIC = 0x49515043;

    // Default ports
    static constexpr int DEFAULT_DATA_PORT = 7373;
    static constexpr int DEFAULT_CTRL_PORT = 7374;

    // Maximum IQ samples per frame (each sample = 1 I + 1 Q value)
    static constexpr uint32_t MAX_SAMPLES_PER_FRAME = 65536;

    // Frame header (sent in network byte order)
    struct FrameHeader {
        uint32_t magic;        // Must be MAGIC
        uint32_t num_samples;  // Number of IQ sample pairs in this frame
    };

} // namespace iq_protocol