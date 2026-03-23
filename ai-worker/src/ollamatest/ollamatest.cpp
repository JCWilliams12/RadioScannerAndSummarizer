#include "ollamatest.hpp"
#include "llama.h"
#include <string>
#include <iostream>
#include <vector>
#include <mutex>

static llama_model* g_model = nullptr;
static std::mutex g_llama_mutex;

std::string GenerateSummary(std::string transcript) {
    std::lock_guard<std::mutex> lock(g_llama_mutex); 

    try {
        if (!g_model) {
            std::cout << "\n[AI] Initializing local Phi-3 Mini engine..." << std::endl;
            llama_backend_init();
            
            llama_model_params model_params = llama_model_default_params();
            // UPDATED: llama_load_model_from_file is now llama_model_load_from_file
            g_model = llama_model_load_from_file("/app/shared/models/Phi-3-mini-4k-instruct-q4.gguf", model_params);            
            if (!g_model) {
                return "[Error] Failed to load Phi-3 model.";
            }
        }

        llama_context_params ctx_params = llama_context_default_params();
        ctx_params.n_ctx = 2048; 
        // UPDATED: llama_new_context_with_model is now llama_init_from_model
        llama_context* ctx = llama_init_from_model(g_model, ctx_params);

        const llama_vocab* vocab = llama_model_get_vocab(g_model);

        std::string full_prompt = 
            "<|user|>\n"
            "Analyze the following intercepted radio transcript. "
            "Categorize it using exactly one of these labels: "
            "[News, Sports, Music, Religion, Talk Radio, Emergency, Advertisement, Unknown]. "
            "Provide a strict 1-2 sentence summary of the core subject. "
            "Do not include conversational filler. Format your exact response like this: "
            "[Category] - [Summary].\n\n"
            "Transcript: " + transcript + "<|end|>\n<|assistant|>\n";

        std::vector<llama_token> tokens_list(full_prompt.length() + 8);
        
        int n_tokens = llama_tokenize(vocab, full_prompt.c_str(), full_prompt.length(), tokens_list.data(), tokens_list.size(), true, true);
        if (n_tokens < 0) {
            tokens_list.resize(-n_tokens);
            n_tokens = llama_tokenize(vocab, full_prompt.c_str(), full_prompt.length(), tokens_list.data(), tokens_list.size(), true, true);
        }
        tokens_list.resize(n_tokens);

        llama_batch batch = llama_batch_init(2048, 0, 1);
        for (size_t i = 0; i < tokens_list.size(); i++) {
            batch.token[batch.n_tokens] = tokens_list[i];
            batch.pos[batch.n_tokens] = i;
            batch.n_seq_id[batch.n_tokens] = 1;
            batch.seq_id[batch.n_tokens][0] = 0;
            batch.logits[batch.n_tokens] = false;
            batch.n_tokens++;
        }
        batch.logits[batch.n_tokens - 1] = true; 

        if (llama_decode(ctx, batch) != 0) {
            llama_batch_free(batch);
            llama_free(ctx); 
            return "[Error] Engine decode failed.";
        }

        std::string result = "";
        int n_cur = batch.n_tokens;

        while (n_cur <= 2048) {
            auto n_vocab_size = llama_vocab_n_tokens(vocab);
            auto * logits = llama_get_logits_ith(ctx, batch.n_tokens - 1);

            llama_token new_token_id = 0;
            float max_logit = -1e9;
            for (int i = 0; i < n_vocab_size; i++) {
                if (logits[i] > max_logit) {
                    max_logit = logits[i];
                    new_token_id = i;
                }
            }

            // UPDATED: llama_token_is_eog is now llama_vocab_is_eog
            if (llama_vocab_is_eog(vocab, new_token_id)) {
                break;
            }

            char buf[128] = {0};
            int n_chars = llama_token_to_piece(vocab, new_token_id, buf, sizeof(buf), 0, true);
            if (n_chars > 0) {
                result += std::string(buf, n_chars);
            }

            batch.n_tokens = 0; 
            batch.token[0] = new_token_id;
            batch.pos[0] = n_cur;
            batch.n_seq_id[0] = 1;
            batch.seq_id[0][0] = 0;
            batch.logits[0] = true;
            batch.n_tokens = 1;

            if (llama_decode(ctx, batch) != 0) {
                break;
            }
            n_cur++;
        }

        llama_batch_free(batch);
        llama_free(ctx);
        
        return result;

    } catch (...) {
        return "[Error] Unknown AI Exception.";
    }
}