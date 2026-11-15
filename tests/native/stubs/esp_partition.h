#pragma once

#include <stddef.h>
#include <stdint.h>

typedef int esp_err_t;
typedef uint8_t esp_partition_type_t;
typedef uint8_t esp_partition_subtype_t;
typedef uint32_t esp_partition_mmap_handle_t;
typedef uint32_t esp_partition_mmap_flag_t;

typedef struct esp_partition_t {
  size_t size;
} esp_partition_t;

enum {
  ESP_OK = 0,
  ESP_PARTITION_TYPE_DATA = 1,
  ESP_PARTITION_MMAP_DATA = 0
};

#ifdef __cplusplus
extern "C" {
#endif

const esp_partition_t *esp_partition_find_first(
    esp_partition_type_t type, esp_partition_subtype_t subtype, const char *label);
esp_err_t esp_partition_read(const esp_partition_t *partition, size_t offset,
                             void *dst, size_t size);
esp_err_t esp_partition_mmap(const esp_partition_t *partition, size_t offset,
                             size_t size, esp_partition_mmap_flag_t memory,
                             const void **out_ptr,
                             esp_partition_mmap_handle_t *out_handle);
void esp_partition_munmap(esp_partition_mmap_handle_t handle);

#ifdef __cplusplus
}
#endif
