// TLS handshakes can briefly need more contiguous memory than the sampler keeps
// internally. mbedTLS normally forces this allocator into internal RAM; route only
// those allocations to PSRAM, which is released by the Wi-Fi UI handoff.
#if defined(KANPLAY_SAMPLER)

#include <stddef.h>

#include <esp_heap_caps.h>

extern "C" void* __wrap_esp_mbedtls_mem_calloc(size_t count, size_t size)
{
  return heap_caps_calloc(count, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}

#endif
