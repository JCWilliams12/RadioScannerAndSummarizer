FROM aetherguard-base:latest
WORKDIR /app/api-core
# Install SQLite and Curl just in case base missed it
RUN apt-get update && apt-get install -y libsqlite3-dev libcurl4-openssl-dev && rm -rf /var/lib/apt/lists/*
# Copy headers to the system path
COPY include/ /usr/local/include/
# Copy ONLY the api-core files
COPY api-core/ .
RUN mkdir -p build && cd build && \
    cmake .. -DCMAKE_BUILD_TYPE=Release && \
    make -j$(nproc)
EXPOSE 8080
CMD ["./build/api_core"]