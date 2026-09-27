#include <stdarg.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "lwip/sockets.h"
#include "lwip/inet.h"

#include "netlog.h"

#define RING_SZ   16384
#define LOG_LINE  320     /* not LINE_MAX: libc already defines that */
#define UDP_MTU   1024

static char           *s_ring;
static size_t          s_head;      /* next write offset */
static uint64_t        s_written;    /* total bytes ever logged */
static portMUX_TYPE    s_mux = portMUX_INITIALIZER_UNLOCKED;
static vprintf_like_t  s_prev;
static volatile bool   s_net_ready;

/* ------------------------------------------------------------------- ring */

static void ring_put(const char *p, size_t n)
{
    if (n > RING_SZ) {            /* keep the tail of an over-long line */
        p += n - RING_SZ;
        n  = RING_SZ;
    }
    portENTER_CRITICAL(&s_mux);
    size_t head = s_head;
    for (size_t i = 0; i < n; i++) {
        s_ring[head] = p[i];
        if (++head == RING_SZ) {
            head = 0;
        }
    }
    s_head    = head;
    s_written += n;
    portEXIT_CRITICAL(&s_mux);
}

/*
 * Installed via esp_log_set_vprintf, so this runs in whatever task called
 * ESP_LOGx. It must not allocate, must not block, and must never log - any
 * of those would deadlock or recurse. Copying into the ring under a short
 * spinlock is all that happens here; the network send is somebody else's job.
 */
static int log_hook(const char *fmt, va_list ap)
{
    va_list ap2;
    va_copy(ap2, ap);

    char line[LOG_LINE];
    int n = vsnprintf(line, sizeof(line), fmt, ap);
    if (n > 0) {
        ring_put(line, (size_t)n < sizeof(line) ? (size_t)n : sizeof(line) - 1);
    }

    /* Keep UART alive underneath: free when nothing is attached, and the only
     * sink that still works during a panic. */
    int r = s_prev ? s_prev(fmt, ap2) : n;
    va_end(ap2);
    return r;
}

uint64_t netlog_cursor(void)
{
    portENTER_CRITICAL(&s_mux);
    uint64_t w = s_written;
    portEXIT_CRITICAL(&s_mux);
    return w;
}

size_t netlog_read(uint64_t from, char *out, size_t max, uint64_t *next)
{
    if (!s_ring || !max) {
        if (next) *next = netlog_cursor();
        return 0;
    }

    portENTER_CRITICAL(&s_mux);
    uint64_t written = s_written;
    size_t   head    = s_head;
    portEXIT_CRITICAL(&s_mux);

    uint64_t oldest = written > RING_SZ ? written - RING_SZ : 0;
    if (from < oldest) {
        from = oldest;          /* reader fell behind; skip the lost span */
    }
    if (from > written) {
        from = written;         /* board rebooted under the reader */
    }

    size_t n = (size_t)(written - from);
    if (n > max) {
        from += n - max;
        n = max;
    }

    /*
     * Copied without the lock. A write landing mid-copy can tear the oldest
     * bytes of the window, which for a log tail is not worth blocking
     * interrupts for 16 KB to prevent.
     */
    size_t start = (head + RING_SZ - (size_t)(written - from)) % RING_SZ;
    for (size_t i = 0; i < n; i++) {
        out[i] = s_ring[(start + i) % RING_SZ];
    }

    if (next) *next = written;
    return n;
}

/* -------------------------------------------------------------------- udp */

void netlog_set_network_ready(bool ready)
{
    s_net_ready = ready;
}

/*
 * Broadcasts new log bytes to the LAN. Deliberately fire-and-forget: no
 * retries, no acknowledgement, and never any logging of its own (which would
 * feed itself). Listen on a PC with the one-liner in the README.
 */
static void udp_task(void *arg)
{
    char *buf = malloc(UDP_MTU);
    if (!buf) {
        vTaskDelete(NULL);
        return;
    }

    int      sock   = -1;
    uint64_t cursor = 0;

    struct sockaddr_in to = {
        .sin_family      = AF_INET,
        .sin_port        = htons(NETLOG_UDP_PORT),
        .sin_addr.s_addr = htonl(INADDR_BROADCAST),
    };

    while (1) {
        if (!s_net_ready) {
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }
        if (sock < 0) {
            sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
            if (sock < 0) {
                vTaskDelay(pdMS_TO_TICKS(1000));
                continue;
            }
            int yes = 1;
            setsockopt(sock, SOL_SOCKET, SO_BROADCAST, &yes, sizeof(yes));
            cursor = 0;   /* send the backlog, including pre-WiFi boot lines */
        }

        uint64_t next = cursor;
        size_t   n    = netlog_read(cursor, buf, UDP_MTU, &next);
        if (n > 0) {
            if (sendto(sock, buf, n, 0, (struct sockaddr *)&to, sizeof(to)) < 0) {
                close(sock);
                sock = -1;
                vTaskDelay(pdMS_TO_TICKS(500));
                continue;
            }
            cursor = next;
            vTaskDelay(pdMS_TO_TICKS(20));   /* don't flood on a big backlog */
        } else {
            vTaskDelay(pdMS_TO_TICKS(200));
        }
    }
}

/* ------------------------------------------------------------------- init */

esp_err_t netlog_start(void)
{
    /* PSRAM keeps 16 KB of internal RAM free for WiFi and USB buffers. */
    s_ring = heap_caps_malloc(RING_SZ, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_ring) {
        s_ring = malloc(RING_SZ);
    }
    if (!s_ring) {
        return ESP_ERR_NO_MEM;
    }
    memset(s_ring, 0, RING_SZ);

    s_prev = esp_log_set_vprintf(log_hook);

    if (xTaskCreate(udp_task, "netlog", 4096, NULL, 2, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}
