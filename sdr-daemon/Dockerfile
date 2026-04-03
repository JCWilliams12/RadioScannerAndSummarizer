FROM aetherguard-base:latest
WORKDIR /app/sdr-daemon

# 1. Install DSP / audio dependencies only — no USB libs needed
RUN apt-get update && apt-get install -y \
    libliquid-dev \
    libsndfile1-dev \
    wget \
    && rm -rf /var/lib/apt/lists/*

# 2. Download AudioFile header
RUN wget https://raw.githubusercontent.com/adamstark/AudioFile/master/AudioFile.h \
    -O /usr/local/include/AudioFile.h

# 3. Copy project files (include/ has shared headers like iq_protocol.hpp)
COPY include/ /usr/local/include/
COPY sdr-daemon/ .

# 4. Build the daemon
RUN mkdir -p build && cd build && \
    cmake .. -DCMAKE_BUILD_TYPE=Release && \
    make -j$(nproc)

# 5. Launch — no sdrplay_apiService needed anymore
CMD ["./build/sdr_daemon"]
