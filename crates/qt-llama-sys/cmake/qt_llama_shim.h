/* C ABI over the C++-only device/memory helpers in llama.cpp's llama-ext.h. */
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "ggml-backend.h"
#include "llama.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct qt_llama_memory_entry {
    ggml_backend_buffer_type_t buffer_type;
    size_t model;
    size_t context;
    size_t compute;
} qt_llama_memory_entry;

/* Number of devices the model was loaded onto. */
int32_t qt_llama_model_n_devices(const struct llama_model * model);

/* Device `index` of the model, or NULL. */
ggml_backend_dev_t qt_llama_model_get_device(const struct llama_model * model, int32_t index);

/*
 * Writes up to `capacity` memory breakdown entries for the context into `out`
 * and returns the total number of entries available.
 */
size_t qt_llama_memory_breakdown(const struct llama_context * context,
                                 qt_llama_memory_entry * out,
                                 size_t capacity);

#ifdef __cplusplus
}
#endif
