/* libFuzzer target: reassembly of fragmented messages (CCID up to ~3 KB) from arbitrary packets.
 * The input is a sequence of [length][packet] records fed to one reassembler. */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "yd_link.h"
#include "yd_frag.h"

esp_err_t yd_link_send(uint8_t ch, const uint8_t *d, uint16_t l)
{
    (void)ch; (void)d; (void)l;
    return ESP_OK;
}

uint16_t yd_link_max_payload(void)
{
    return 222;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    static uint8_t buf[3136];
    yd_frag_rx_t r;
    yd_frag_rx_init(&r, buf, sizeof(buf));
    size_t off = 0;
    while (off < size) {
        size_t n = data[off++];
        if (n > size - off) {
            n = size - off;
        }
        int got = yd_frag_rx_feed(&r, data + off, (uint16_t)n);
        if (got > (int)sizeof(buf) || r.len > r.cap) {
            abort();
        }
        off += n;
    }
    return 0;
}
