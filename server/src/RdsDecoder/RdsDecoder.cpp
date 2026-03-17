#include "RdsDecoder.hpp"
#include <cmath>
#include <iostream>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

std::string RdsDecoder::ExtractName(const std::vector<float>& iq_buffer, float sampleRate) {
    if (iq_buffer.empty()) return "UNKNOWN";

    std::cout << "[DSP] Running FM Demodulation..." << std::endl;
    std::vector<float> fm_baseband = FmDemodulate(iq_buffer);

    std::cout << "[DSP] Applying Biquad Bandpass Filter (57 kHz)..." << std::endl;
    std::vector<float> rds_tone = BandpassFilter57kHz(fm_baseband, sampleRate);

    std::cout << "[DSP] Locking BPSK Costas Loop (DPLL)..." << std::endl;
    std::vector<int> raw_bits = BpskCostasLoop(rds_tone, sampleRate);

    std::cout << "[DSP] Decoding Data Blocks..." << std::endl;
    return DecodeBitsToText(raw_bits);
}

// -------------------------------------------------------------------
// STEP 1: FM Demodulator (Polar Discriminator)
// -------------------------------------------------------------------
std::vector<float> RdsDecoder::FmDemodulate(const std::vector<float>& iq_buffer) {
    std::vector<float> fm_audio;
    fm_audio.reserve(iq_buffer.size() / 2);

    float prev_i = 0.0f;
    float prev_q = 0.0f;

    // Iterate through pairs of I and Q
    for (size_t i = 0; i < iq_buffer.size(); i += 2) {
        float curr_i = iq_buffer[i];
        float curr_q = iq_buffer[i + 1];

        // Complex multiplication of current sample by conjugate of previous sample
        // delta_phase = atan2(I*Q_prev - Q*I_prev, I*I_prev + Q*Q_prev)
        float real_part = (curr_i * prev_i) + (curr_q * prev_q);
        float imag_part = (curr_q * prev_i) - (curr_i * prev_q);

        float phase_diff = std::atan2(imag_part, real_part);
        fm_audio.push_back(phase_diff);

        prev_i = curr_i;
        prev_q = curr_q;
    }
    return fm_audio;
}

// -------------------------------------------------------------------
// STEP 2: FIR Bandpass Filter (Centered at 57 kHz)
// -------------------------------------------------------------------
std::vector<float> RdsDecoder::BandpassFilter57kHz(const std::vector<float>& fm_baseband, float sampleRate) {
    std::vector<float> filtered(fm_baseband.size(), 0.0f);

    // Calculate Biquad Coefficients
    float w0 = 2.0f * M_PI * 57000.0f / sampleRate;
    float alpha = std::sin(w0) / (2.0f * 10.0f); // Q = 10 (Allows ~5.7 kHz bandwidth)

    float b0 = alpha;
    float b1 = 0.0f;
    float b2 = -alpha;
    float a0 = 1.0f + alpha;
    float a1 = -2.0f * std::cos(w0);
    float a2 = 1.0f - alpha;

    // Normalize coefficients
    b0 /= a0; b1 /= a0; b2 /= a0;
    a1 /= a0; a2 /= a0;

    // Filter state history
    float x1 = 0.0f, x2 = 0.0f;
    float y1 = 0.0f, y2 = 0.0f;

    for (size_t i = 0; i < fm_baseband.size(); i++) {
        float x0 = fm_baseband[i];
        
        // The Biquad Difference Equation
        float y0 = b0*x0 + b1*x1 + b2*x2 - a1*y1 - a2*y2;

        filtered[i] = y0;

        // Shift history
        x2 = x1; x1 = x0;
        y2 = y1; y1 = y0;
    }
    
    return filtered;
}

// -------------------------------------------------------------------
// STEP 3: BPSK Costas Loop
// -------------------------------------------------------------------
std::vector<int> RdsDecoder::BpskCostasLoop(const std::vector<float>& rds_tone, float sampleRate) {
    std::vector<int> bits;
    
    // Carrier Phase Tracking
    float phase = 0.0f;
    float freq = (57000.0f * 2.0f * M_PI) / sampleRate; 
    float alpha = 0.01f;  
    float beta = 0.0001f; 

    // Symbol Timing (DPLL)
    float samples_per_symbol = sampleRate / 1187.5f; 
    float ideal_midpoint = samples_per_symbol / 2.0f;
    float symbol_counter = 0.0f;
    
    float integrate_early = 0.0f;
    float integrate_late = 0.0f;
    int last_sign = 0;

    // Baseband LPF to kill the music
    float lpf_i = 0.0f, lpf_q = 0.0f;
    float lpf_alpha = (2400.0f * 2.0f * M_PI) / sampleRate;
    if (lpf_alpha > 1.0f) lpf_alpha = 1.0f;

    for (size_t i = 0; i < rds_tone.size(); i++) {
        
        // 1. Costas Carrier Mix
        float local_i = std::cos(phase);
        float local_q = -std::sin(phase);

        float mix_i = rds_tone[i] * local_i;
        float mix_q = rds_tone[i] * local_q;

        lpf_i += lpf_alpha * (mix_i - lpf_i);
        lpf_q += lpf_alpha * (mix_q - lpf_q);

        float error = lpf_i * (lpf_q > 0.0f ? 1.0f : -1.0f);
        freq += beta * error;
        phase += freq + (alpha * error);

        while (phase > M_PI) phase -= 2.0f * M_PI;
        while (phase < -M_PI) phase += 2.0f * M_PI;

        // 2. Integration
        if (symbol_counter < ideal_midpoint) {
            integrate_early += lpf_i;
        } else {
            integrate_late += lpf_i;
        }

        // 3. The Zero-Crossing DPLL (Clock Sync)
        int current_sign = (lpf_i > 0.0f) ? 1 : -1;
        symbol_counter += 1.0f;

        if (current_sign != last_sign) {
            // We found a zero-crossing! 
            // In Manchester encoding, the crossing near the middle of the bit is guaranteed.
            // If it happens in the middle 50% of our window, it's the sync crossing.
            if (symbol_counter > (samples_per_symbol * 0.25f) && symbol_counter < (samples_per_symbol * 0.75f)) {
                
                // Calculate how far off our internal clock was from the physical wave
                float timing_error = symbol_counter - ideal_midpoint;
                
                // Aggressively nudge the clock to snap to the wave (makes us immune to USB jitter!)
                symbol_counter -= 0.15f * timing_error; 
            }
        }

        // 4. Dump and Decide (End of bit)
        if (symbol_counter >= samples_per_symbol) {
            bits.push_back((integrate_early - integrate_late) > 0 ? 1 : 0);
            
            integrate_early = 0.0f;
            integrate_late = 0.0f;
            symbol_counter -= samples_per_symbol;
        }

        last_sign = current_sign;
    }
    
    return bits;
}

// -------------------------------------------------------------------
// STEP 4: Differential Decoding & Block Sync
// -------------------------------------------------------------------
std::string RdsDecoder::DecodeBitsToText(const std::vector<int>& bits) {
    if (bits.size() < 104) return "UNKNOWN";

    // 1. Differential Decode (Fixes 180-degree phase ambiguity from the Costas Loop)
    std::vector<int> diff_bits(bits.size(), 0);
    for (size_t i = 1; i < bits.size(); i++) {
        diff_bits[i] = bits[i] ^ bits[i - 1];
    }

    // 2. The FCC Syndrome Calculator (Error Detection Math)
    // Uses the standard RDS Generator Polynomial: x^10 + x^8 + x^7 + x^5 + x^4 + x^3 + 1 (0x5B9)
    auto calc_syndrome = [](uint32_t block26) -> uint32_t {
        uint32_t reg = block26 & 0x3FFFFFF; // 26 bits
        for (int i = 0; i < 16; i++) {
            if (reg & 0x2000000) { // If the 26th bit is 1
                reg ^= (0x5B9 << 15); // XOR with the polynomial
            }
            reg <<= 1;
        }
        return (reg >> 16) & 0x3FF; // Return the 10-bit remainder
    };

    // Standard FCC Offset Words
    const uint32_t SYNC_A = 0x0FC;
    const uint32_t SYNC_B = 0x198;

    char ps_name[9] = "        "; // 8 spaces + null terminator
    bool name_found = false;

    // 3. Sliding Window Frame Synchronizer
    uint32_t bit_window = 0;
    for (size_t i = 0; i < diff_bits.size(); i++) {
        bit_window = ((bit_window << 1) | diff_bits[i]) & 0x3FFFFFF;

        if (i < 25) continue; // Wait until we have a full 26-bit block

        // Check if our window perfectly aligned with Block A
        if (calc_syndrome(bit_window) == SYNC_A) {
            
            // If A is found, fast-forward to check B and D
            if (i + 78 < diff_bits.size()) {
                uint32_t block_b = 0, block_d = 0;
                for (int j = 1; j <= 26; j++)  block_b = (block_b << 1) | diff_bits[i + j];
                for (int j = 53; j <= 78; j++) block_d = (block_d << 1) | diff_bits[i + j];

                // Verify Block B is valid
                if (calc_syndrome(block_b) == SYNC_B) {
                    
                    uint16_t data_b = (block_b >> 10) & 0xFFFF;
                    uint16_t data_d = (block_d >> 10) & 0xFFFF;

                    // Group Type is the top 4 bits of Block B. 
                    // Group 0 (0A or 0B) contains the Station Name!
                    uint8_t group_type = (data_b >> 12) & 0xF;
                    
                    if (group_type == 0) {
                        // The bottom 2 bits of Block B tell us which 2 characters these are (0-3)
                        uint8_t segment_address = data_b & 0x0003;
                        
                        // Block D contains the actual text characters
                        char char1 = (data_d >> 8) & 0xFF;
                        char char2 = data_d & 0xFF;

                        // Only save printable ASCII characters
                        if (char1 >= 32 && char1 <= 126) ps_name[segment_address * 2] = char1;
                        if (char2 >= 32 && char2 <= 126) ps_name[segment_address * 2 + 1] = char2;
                        
                        name_found = true;
                    }
                }
                i += 78; // Skip ahead since we just processed a full 104-bit frame
            }
        }
    }
    
    // 4. Format the final output
    std::string result(ps_name);
    
    // Trim any trailing blank spaces
    result.erase(result.find_last_not_of(' ') + 1);
    
    return result.empty() || !name_found ? "UNKNOWN" : result;
}