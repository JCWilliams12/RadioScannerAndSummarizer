FROM aetherguard-base:latest
WORKDIR /app/sdr-daemon

# 1. Install system dependencies
RUN apt-get update && apt-get install -y \
    libusb-1.0-0-dev \
    libudev-dev \
    libliquid-dev \
    libsndfile1-dev \
    wget \
    && rm -rf /var/lib/apt/lists/*

# 2. Extract SDRplay API manually (Bypasses the "expect" and interactive prompts)
COPY SDRplay_RSP_API-Linux-3.15.2.run /tmp/
RUN chmod +x /tmp/SDRplay_RSP_API-Linux-3.15.2.run && \
    cd /tmp && \
    # Extract the archive without running the interactive script
    ./SDRplay_RSP_API-Linux-3.15.2.run --tar xvf && \
    # Move the files to system paths (using amd64 as per the 3.15 standard)
    cp amd64/libsdrplay_api.so.3.15 /usr/local/lib/ && \
    ln -sf /usr/local/lib/libsdrplay_api.so.3.15 /usr/local/lib/libsdrplay_api.so && \
    ln -sf /usr/local/lib/libsdrplay_api.so.3.15 /usr/local/lib/libsdrplay_api.so.3 && \
    cp inc/* /usr/local/include/ && \
    mkdir -p /opt/sdrplay_api && \
    cp amd64/sdrplay_apiService /opt/sdrplay_api/ && \
    chmod +x /opt/sdrplay_api/sdrplay_apiService && \
    ldconfig && \
    rm -rf /tmp/*

# 3. Download AudioFile header
RUN wget https://raw.githubusercontent.com/adamstark/AudioFile/master/AudioFile.h -O /usr/local/include/AudioFile.h

# 4. Copy project files
COPY include/ /usr/local/include/
COPY sdr-daemon/ .

# 5. Build the daemon
RUN mkdir -p build && cd build && \
    cmake .. -DCMAKE_BUILD_TYPE=Release && \
    make -j$(nproc)

# 6. Start the Service and the Daemon (Using absolute path to service)
CMD sh -c "/opt/sdrplay_api/sdrplay_apiService & sleep 2 && ./build/sdr_daemon"