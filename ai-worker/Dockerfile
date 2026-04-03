FROM aetherguard-base:latest
WORKDIR /app/ai-worker

RUN git clone https://github.com/ggerganov/whisper.cpp.git /app/whisper && \
    cd /app/whisper && \
    cmake -B build -DBUILD_SHARED_LIBS=OFF && \
    cmake --build build --config Release -j$(nproc)

RUN git clone https://github.com/ggerganov/llama.cpp.git /app/llama && \
    cd /app/llama && \
    cmake -B build -DBUILD_SHARED_LIBS=OFF && \
    cmake --build build --config Release -j$(nproc)

COPY include/ /usr/local/include/
COPY ai-worker/ /app/ai-worker/

RUN g++ -O3 src/main.cpp src/ollamatest/ollamatest.cpp src/whispertinytest/whispertinytest.cpp src/openai/openai.cpp \
    -o ai_worker -lpthread -lhiredis -lcurl -fopenmp \
    -Isrc -Isrc/ollamatest -Isrc/whispertinytest -Isrc/openai -I/usr/local/include/ \
    -I/app/whisper/include -I/app/whisper/ggml/include \
    -I/app/llama/include -I/app/llama/ggml/include \
    -L/app/whisper/build/src \
    -L/app/llama/build/src \
    -L/app/llama/build/ggml/src \
    -lwhisper \
    -lllama \
    -Wl,--start-group -lllama -lggml -lggml-base -lggml-cpu -Wl,--end-group \
    -lstdc++ -lm

CMD ["./ai_worker"]