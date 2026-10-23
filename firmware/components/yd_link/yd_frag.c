#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "yd_link.h"
#include "yd_frag.h"
#include "yd_lang.h"

static const char *TAG = "yd_frag";
static SemaphoreHandle_t s_tx_mutex;

void yd_frag_init(void)
{
    if (!s_tx_mutex) {
        s_tx_mutex = xSemaphoreCreateMutex();
    }
}

void yd_frag_rx_init(yd_frag_rx_t *r, uint8_t *buf, size_t cap)
{
    memset(r, 0, sizeof(*r));
    r->buf = buf;
    r->cap = cap;
}

void yd_frag_rx_reset(yd_frag_rx_t *r)
{
    r->len = 0;
    r->next_seq = 0;
    r->active = false;
}

int yd_frag_rx_feed(yd_frag_rx_t *r, const uint8_t *p, uint16_t n)
{
    if (n < YD_FRAG_HDR) {
        return -1;
    }
    uint8_t flags = p[0], seq = p[1];
    const uint8_t *d = p + YD_FRAG_HDR;
    size_t dn = n - YD_FRAG_HDR;

    if (flags & YD_FRAG_START) {
        if (r->active) {
            r->dropped++;            /* 前のメッセージが最後まで届かなかった */
        }
        r->len = 0;
        r->next_seq = 0;
        r->active = true;
    }
    if (!r->active) {
        return 0;                    /* 先頭片を見ていない（途中から受信）→ 黙って捨てる */
    }
    if (seq != r->next_seq || r->len + dn > r->cap) {
        r->dropped++;
        yd_frag_rx_reset(r);
        return -1;
    }
    memcpy(r->buf + r->len, d, dn);
    r->len += dn;
    r->next_seq++;
    if (flags & YD_FRAG_END) {
        r->active = false;
        return (int)r->len;
    }
    return 0;
}

esp_err_t yd_frag_send(uint8_t ch, const uint8_t *msg, size_t len)
{
    uint16_t maxp = yd_link_max_payload();
    if (maxp <= YD_FRAG_HDR) {
        return ESP_ERR_INVALID_STATE;
    }
    size_t chunk = maxp - YD_FRAG_HDR;
    if (chunk > YD_FRAG_MAX_CHUNK) {
        chunk = YD_FRAG_MAX_CHUNK;
    }
    if (len == 0 || (len + chunk - 1) / chunk > 256) {
        return ESP_ERR_INVALID_SIZE;
    }
    uint8_t pkt[YD_FRAG_HDR + YD_FRAG_MAX_CHUNK];
    esp_err_t err = ESP_OK;
    if (s_tx_mutex) {
        xSemaphoreTake(s_tx_mutex, portMAX_DELAY);
    }
    size_t off = 0;
    uint8_t seq = 0;
    while (off < len) {
        size_t n = len - off > chunk ? chunk : len - off;
        pkt[0] = (off == 0 ? YD_FRAG_START : 0) | (off + n == len ? YD_FRAG_END : 0);
        pkt[1] = seq++;
        memcpy(pkt + YD_FRAG_HDR, msg + off, n);
        /* 送信キューが一杯のときは少し待って再試行（3KB で16片程度なので普通は詰まらない） */
        for (int retry = 0; retry < 20; retry++) {
            err = yd_link_send(ch, pkt, (uint16_t)(n + YD_FRAG_HDR));
            if (err != ESP_ERR_TIMEOUT) {
                break;
            }
        }
        if (err != ESP_OK) {
            ESP_LOGW(TAG, YD_L("ch%u: send failed at byte %u/%u (%s)", "ch%u: %u/%u バイト目で送信失敗 (%s)"), ch, (unsigned)off, (unsigned)len, esp_err_to_name(err));
            break;
        }
        off += n;
    }
    if (s_tx_mutex) {
        xSemaphoreGive(s_tx_mutex);
    }
    return err;
}
