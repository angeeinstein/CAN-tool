// TCP server on TCP_PORT with the same protocol as USB. One client at a time.
//
// Own task: accepts, reads command lines (-> command queue, SINK_TCP) and closes dead connections.
// TCP keepalive (10 s idle, 3 probes 5 s apart) detects a client that vanished without closing the
// connection (laptop asleep, router power loss) after ~25 s, so the slot is free again for its
// reconnect and the ACTIVE watchdog sees that the controlling client is gone.
#include <atomic>
#include <errno.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <lwip/sockets.h>
#include <stdio.h>
#include <string.h>

#include "commands.h"
#include "config.h"
#include "host_io.h"
#include "net.h"
#include "sys.h"

namespace {

SemaphoreHandle_t g_mtx = nullptr;   // guards g_fd for writers vs. close
volatile int g_fd = -1;
volatile bool g_drop = false;        // a writer saw a fatal send error -> the task closes the socket
std::atomic<uint32_t> g_dropped{0};

void close_client() {
    xSemaphoreTake(g_mtx, portMAX_DELAY);
    if (g_fd >= 0) {
        shutdown(g_fd, SHUT_RDWR);
        close(g_fd);
    }
    g_fd = -1;
    g_drop = false;
    xSemaphoreGive(g_mtx);
}

void setup_client_socket(int fd) {
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &one, sizeof(one));
    int idle = 10, intvl = 5, cnt = 3;
    setsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE, &idle, sizeof(idle));
    setsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL, &intvl, sizeof(intvl));
    setsockopt(fd, IPPROTO_TCP, TCP_KEEPCNT, &cnt, sizeof(cnt));
    timeval tv = {2, 0};   // replies: wait at most 2 s per send for a slow client
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
}

void tcp_task(void*) {
    const int lfd = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    int one = 1;
    setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(TCP_PORT);
    if (bind(lfd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 || listen(lfd, 1) != 0) {
        host_notify("#error tcp server: cannot listen on port %d (errno %d)", TCP_PORT, errno);
        vTaskDelete(nullptr);
        return;
    }

    LineBuffer line;
    char buf[256];
    for (;;) {
        if (g_drop) close_client();
        const int cfd = g_fd;
        fd_set rd;
        FD_ZERO(&rd);
        FD_SET(lfd, &rd);
        if (cfd >= 0) FD_SET(cfd, &rd);
        timeval tv = {0, 500 * 1000};
        const int r = select((cfd > lfd ? cfd : lfd) + 1, &rd, nullptr, nullptr, &tv);
        if (r <= 0) continue;

        if (FD_ISSET(lfd, &rd)) {
            const int nfd = accept(lfd, nullptr, nullptr);
            if (nfd >= 0) {
                if (g_fd >= 0) {
                    static const char busy[] = "#err only one TCP client allowed\n\r";
                    send(nfd, busy, sizeof(busy) - 1, MSG_DONTWAIT);
                    close(nfd);
                } else {
                    setup_client_socket(nfd);
                    line.reset();
                    xSemaphoreTake(g_mtx, portMAX_DELAY);
                    g_fd = nfd;
                    g_drop = false;
                    xSemaphoreGive(g_mtx);
                }
            }
        }
        if (cfd >= 0 && FD_ISSET(cfd, &rd)) {
            const int n = recv(cfd, buf, sizeof(buf), 0);
            if (n <= 0) {   // closed by the client, reset, or keepalive timeout
                close_client();
                continue;
            }
            for (int i = 0; i < n; i++)
                if (line.feed(buf[i])) cmd_post(SINK_TCP, 0, line.line(), portMAX_DELAY);
        }
    }
}

}  // namespace

void net_tcp_init() {
    g_mtx = xSemaphoreCreateMutex();
    xTaskCreatePinnedToCore(tcp_task, "tcp_srv", 3072, nullptr, 5, nullptr, 0);
}

void net_tcp_write(const char* buf, size_t len, bool reply) {
    if (g_fd < 0 || !g_mtx || len == 0) return;
    // frames: never wait for the lock (the output task must not stall); replies may wait a little
    if (xSemaphoreTake(g_mtx, reply ? pdMS_TO_TICKS(100) : 0) != pdTRUE) {
        if (!reply) g_dropped += len;
        return;
    }
    const int fd = g_fd;
    if (fd >= 0 && !g_drop) {
        if (!reply) {
            // MSG_DONTWAIT: a stalled client must never block the output task (and thus USB)
            const ssize_t n = send(fd, buf, len, MSG_DONTWAIT);
            if (n < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) g_dropped += len;
                else g_drop = true;
            } else if (static_cast<size_t>(n) < len) {
                g_dropped += len - static_cast<size_t>(n);
            }
        } else {
            // reply: everything must arrive (e.g. a #replay of 1000 frames) -> blocking, SO_SNDTIMEO 2 s
            size_t off = 0;
            while (off < len) {
                const ssize_t n = send(fd, buf + off, len - off, 0);
                if (n <= 0) {
                    g_drop = true;   // client too slow or gone: it reconnects and fetches with #replay
                    break;
                }
                off += static_cast<size_t>(n);
            }
        }
    }
    xSemaphoreGive(g_mtx);
}

bool net_tcp_connected() { return g_fd >= 0 && !g_drop; }
uint32_t net_tcp_dropped() { return g_dropped; }

void net_tcp_status(char* out, size_t cap) {
    snprintf(out, cap, " tcp_client=%s tcp_dropped=%lu", net_tcp_connected() ? "yes" : "no",
             (unsigned long)g_dropped);
}
