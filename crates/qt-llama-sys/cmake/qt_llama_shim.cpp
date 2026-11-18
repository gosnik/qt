#include "qt_llama_shim.h"

#include "llama-ext.h"

extern "C" int32_t qt_llama_model_n_devices(const struct llama_model * model) {
    if (model == nullptr) {
        return 0;
    }
    return llama_model_n_devices(model);
}

extern "C" ggml_backend_dev_t qt_llama_model_get_device(const struct llama_model * model,
                                                         int32_t index) {
    if (model == nullptr || index < 0 || index >= llama_model_n_devices(model)) {
        return nullptr;
    }
    return llama_model_get_device(model, index);
}

extern "C" size_t qt_llama_memory_breakdown(const struct llama_context * context,
                                            qt_llama_memory_entry * out,
                                            size_t capacity) {
    if (context == nullptr) {
        return 0;
    }
    try {
        const llama_memory_breakdown breakdown = llama_get_memory_breakdown(context);
        size_t index = 0;
        for (const auto & entry : breakdown) {
            if (out != nullptr && index < capacity) {
                out[index].buffer_type = entry.first;
                out[index].model = entry.second.model;
                out[index].context = entry.second.context;
                out[index].compute = entry.second.compute;
            }
            ++index;
        }
        return index;
    } catch (...) {
        return 0;
    }
}
