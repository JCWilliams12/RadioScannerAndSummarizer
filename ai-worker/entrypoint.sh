#!/bin/bash
# =============================================================================
# ai-worker/entrypoint.sh — Auto-download AI models on first run
# =============================================================================
# Checks for required model files in the shared volume. Downloads any that
# are missing. Models persist across container restarts since they live on
# the Docker volume (./shared-data/models/).
# =============================================================================

set -e

MODEL_DIR="/app/shared/models"
mkdir -p "$MODEL_DIR"

# ---- Whisper base.en (speech-to-text, ~148 MB) ----
WHISPER_MODEL="$MODEL_DIR/ggml-base.en.bin"
WHISPER_URL="https://huggingface.co/ggerganov/whisper.cpp/resolve/main/ggml-base.en.bin"

if [ -f "$WHISPER_MODEL" ]; then
    echo "[AI Setup] Whisper model found: $WHISPER_MODEL"
else
    echo "[AI Setup] Downloading Whisper base.en model (~148 MB)..."
    echo "[AI Setup] This only happens once — model persists in shared-data/models/"
    wget -q --show-progress -O "$WHISPER_MODEL.tmp" "$WHISPER_URL"
    mv "$WHISPER_MODEL.tmp" "$WHISPER_MODEL"
    echo "[AI Setup] Whisper model downloaded successfully."
fi

# ---- Phi-3 Mini Q4 (text summarization, ~2.3 GB) ----
PHI3_MODEL="$MODEL_DIR/Phi-3-mini-4k-instruct-q4.gguf"
PHI3_URL="https://huggingface.co/microsoft/Phi-3-mini-4k-instruct-gguf/resolve/main/Phi-3-mini-4k-instruct-q4.gguf"

if [ -f "$PHI3_MODEL" ]; then
    echo "[AI Setup] Phi-3 model found: $PHI3_MODEL"
else
    echo "[AI Setup] Downloading Phi-3 Mini Q4 model (~2.3 GB)..."
    echo "[AI Setup] This only happens once — model persists in shared-data/models/"
    wget -q --show-progress -O "$PHI3_MODEL.tmp" "$PHI3_URL"
    mv "$PHI3_MODEL.tmp" "$PHI3_MODEL"
    echo "[AI Setup] Phi-3 model downloaded successfully."
fi

echo "[AI Setup] All models ready."
echo ""

# Hand off to the actual worker binary
exec ./ai_worker