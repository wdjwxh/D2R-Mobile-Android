// main+0x1b0ee60 的 DecodeZlib 剪口（svc #0x80），ABI 见上游 hle_tact.c。
// 与上游使用同一 libdeflate 解码器及每线程生命周期；预取清单尚未接入。
#include "dm_host.h"
#include <stdint.h>
#include <string.h>
#include <pthread.h>
#include "../../d2r-mobile/dm/third_party/libdeflate/libdeflate.h"
#include "nvn/nvn_dirty.h"

static pthread_key_t decoder_key;
static pthread_once_t decoder_once = PTHREAD_ONCE_INIT;
static void decoder_free(void *p) { libdeflate_free_decompressor(p); }
static void decoder_key_init(void) {
    if (pthread_key_create(&decoder_key, decoder_free)) die("libdeflate 解压器键创建失败");
}
static struct libdeflate_decompressor *thread_decoder(void) {
    pthread_once(&decoder_once, decoder_key_init);
    struct libdeflate_decompressor *d = pthread_getspecific(decoder_key);
    if (!d) {
        d = libdeflate_alloc_decompressor();
        if (!d) die("libdeflate 分配解压器失败");
        if (pthread_setspecific(decoder_key, d)) {
            libdeflate_free_decompressor(d);
            die("libdeflate 解压器线程登记失败");
        }
    }
    return d;
}

uint32_t tact_decode_zlib(const uint64_t *r) {
    uint8_t *out = (uint8_t *)(uintptr_t)r[0];
    uint32_t cap = (uint32_t)r[1];
    const uint8_t *in = (const uint8_t *)(uintptr_t)r[2];
    uint32_t n = (uint32_t)r[3], flags = (uint32_t)r[4];
    void *override = (void *)(uintptr_t)r[6];
    dm_guest_write_prep(out, cap);
    if (override) {
        typedef uint32_t (*callback)(void *, void *, uint32_t, const void *, uint32_t, uint32_t);
        callback cb = (callback)(*(void ***)override)[2];
        if (!(cb(override, out, cap, in, n, 1) & 0xff)) return 0;
    }
    if (flags & 1) {
        if (n < 2 || in[0] != 'Z') return 8;
        in++; n--;
    }
    size_t got = 0;
    enum libdeflate_result rc = libdeflate_zlib_decompress(thread_decoder(), in, n, out, cap, &got);
    // Same mapping as upstream; preserve zlib header/checksum validation and
    // allow decoded output shorter than capacity by passing actual_out_nbytes.
    return rc == LIBDEFLATE_SUCCESS ? 0 : rc == LIBDEFLATE_INSUFFICIENT_SPACE ? 4 : 1;
}
