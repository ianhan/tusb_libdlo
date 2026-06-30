#include "typewriter_net.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#ifndef PICO_TV_TYPEWRITER_WIFI
#define PICO_TV_TYPEWRITER_WIFI 0
#endif

#if PICO_TV_TYPEWRITER_WIFI

#include <ctype.h>
#include <limits.h>
#include <stdlib.h>

#include "hardware/sync.h"
#include "pico/cyw43_arch.h"
#include "pico/time.h"

#include "lwip/dns.h"
#include "lwip/ip_addr.h"
#include "lwip/netif.h"
#include "lwip/pbuf.h"
#include "lwip/tcp.h"
#include "libtelnet.h"

#define NET_OUTPUT_SIZE 16384u
#define WIFI_CONNECT_TIMEOUT_MS 30000u
#define TELNET_DEFAULT_PORT 23u
#define TELNET_TERMINAL_TYPE "xterm-256color"
#define TELNET_WINDOW_COLS 80u
#define TELNET_WINDOW_ROWS 30u

typedef enum telnet_state_e {
    TELNET_IDLE,
    TELNET_RESOLVING,
    TELNET_CONNECTING,
    TELNET_CONNECTED,
} telnet_state_t;

static typewriter_net_writer_t output_writer;
static char output_ring[NET_OUTPUT_SIZE];
static volatile uint16_t output_head;
static volatile uint16_t output_tail;

static bool wifi_ready;
static bool wifi_scan_running;
static bool wifi_connecting;
static int wifi_last_status = INT_MIN;
static absolute_time_t wifi_connect_deadline;

static telnet_state_t telnet_state = TELNET_IDLE;
static telnet_t *telnet;
static struct tcp_pcb *telnet_pcb;
static uint16_t telnet_port = TELNET_DEFAULT_PORT;
static uint32_t telnet_generation;
static char telnet_host[96];

static const telnet_telopt_t telnet_options[] = {
    { TELNET_TELOPT_TTYPE, TELNET_WILL, TELNET_DONT },
    { TELNET_TELOPT_NAWS, TELNET_WILL, TELNET_DONT },
    { TELNET_TELOPT_ECHO, TELNET_WONT, TELNET_DO },
    { TELNET_TELOPT_SGA, TELNET_WONT, TELNET_DO },
    { -1, 0, 0 },
};

static uint16_t ring_next(uint16_t value) {
    return (uint16_t)((value + 1u) % NET_OUTPUT_SIZE);
}

static void output_enqueue(const char *data, size_t len) {
    if (!data || !len) {
        return;
    }

    uint32_t flags = save_and_disable_interrupts();
    for (size_t i = 0; i < len; ++i) {
        uint16_t next = ring_next(output_head);
        if (next == output_tail) {
            break;
        }
        output_ring[output_head] = data[i];
        output_head = next;
    }
    restore_interrupts(flags);
}

static void output_printf(const char *fmt, ...) {
    char buffer[192];
    va_list args;
    va_start(args, fmt);
    int len = vsnprintf(buffer, sizeof(buffer), fmt, args);
    va_end(args);
    if (len <= 0) {
        return;
    }
    if ((size_t)len >= sizeof(buffer)) {
        len = (int)sizeof(buffer) - 1;
    }
    output_enqueue(buffer, (size_t)len);
}

static void output_drain(void) {
    if (!output_writer) {
        return;
    }

    char buffer[128];
    while (true) {
        size_t len = 0;
        uint32_t flags = save_and_disable_interrupts();
        while (output_tail != output_head && len < sizeof(buffer)) {
            buffer[len++] = output_ring[output_tail];
            output_tail = ring_next(output_tail);
        }
        restore_interrupts(flags);

        if (!len) {
            break;
        }
        output_writer(buffer, len);
    }
}

static const char *wifi_status_name(int status) {
    switch (status) {
    case CYW43_LINK_DOWN: return "down";
    case CYW43_LINK_JOIN: return "joined";
    case CYW43_LINK_NOIP: return "waiting for ip";
    case CYW43_LINK_UP: return "up";
    case CYW43_LINK_FAIL: return "failed";
    case CYW43_LINK_NONET: return "network not found";
    case CYW43_LINK_BADAUTH: return "bad password";
    default: return "unknown";
    }
}

static const char *wifi_auth_name(uint8_t auth_mode) {
    if (auth_mode == CYW43_AUTH_OPEN) {
        return "open";
    }
    if (auth_mode & 0x04u) {
        return "wpa2";
    }
    if (auth_mode & 0x02u) {
        return "wpa";
    }
    return "secured";
}

static void wifi_print_ip(void) {
    char ip[24] = "0.0.0.0";
    cyw43_arch_lwip_begin();
    if (netif_default) {
        ip4addr_ntoa_r(netif_ip4_addr(netif_default), ip, sizeof(ip));
    }
    cyw43_arch_lwip_end();
    output_printf("wifi: ip %s\r\n", ip);
}

static int wifi_scan_result(void *env, const cyw43_ev_scan_result_t *result) {
    (void)env;
    if (!result) {
        return 0;
    }

    char ssid[33];
    size_t ssid_len = result->ssid_len;
    if (ssid_len > sizeof(result->ssid)) {
        ssid_len = sizeof(result->ssid);
    }
    for (size_t i = 0; i < ssid_len; ++i) {
        uint8_t c = result->ssid[i];
        ssid[i] = isprint(c) ? (char)c : '.';
    }
    ssid[ssid_len] = '\0';

    output_printf("%-32s ch=%2u rssi=%4d %s\r\n",
                  ssid_len ? ssid : "<hidden>",
                  (unsigned)result->channel,
                  (int)result->rssi,
                  wifi_auth_name(result->auth_mode));
    return 0;
}

static bool wifi_link_is_up(void) {
    return wifi_ready && cyw43_tcpip_link_status(&cyw43_state, CYW43_ITF_STA) == CYW43_LINK_UP;
}

static void wifi_poll_connect(void) {
    if (!wifi_connecting) {
        return;
    }

    int status = cyw43_tcpip_link_status(&cyw43_state, CYW43_ITF_STA);
    if (status != wifi_last_status) {
        wifi_last_status = status;
        output_printf("wifi: %s\r\n", wifi_status_name(status));
    }

    if (status == CYW43_LINK_UP) {
        wifi_connecting = false;
        wifi_print_ip();
        return;
    }

    if (status < 0) {
        wifi_connecting = false;
        return;
    }

    if (time_reached(wifi_connect_deadline)) {
        wifi_connecting = false;
        output_enqueue("wifi: timed out\r\n", 17);
    }
}

static void telnet_reset(void) {
    if (telnet) {
        telnet_free(telnet);
        telnet = NULL;
    }
    telnet_state = TELNET_IDLE;
    telnet_port = TELNET_DEFAULT_PORT;
    telnet_host[0] = '\0';
}

static void telnet_send_window_size(void) {
    if (!telnet) {
        return;
    }

    const char size[] = {
        (uint8_t)(TELNET_WINDOW_COLS >> 8u),
        (uint8_t)TELNET_WINDOW_COLS,
        (uint8_t)(TELNET_WINDOW_ROWS >> 8u),
        (uint8_t)TELNET_WINDOW_ROWS,
    };

    telnet_begin_sb(telnet, TELNET_TELOPT_NAWS);
    telnet_send(telnet, size, sizeof(size));
    telnet_finish_sb(telnet);
}

static void telnet_write_network(const char *data, size_t len) {
    if (!telnet_pcb || !data || !len) {
        return;
    }

    err_t err = tcp_write(telnet_pcb, data, len, TCP_WRITE_FLAG_COPY);
    if (err == ERR_OK) {
        err = tcp_output(telnet_pcb);
    }
    if (err != ERR_OK) {
        output_printf("\r\ntelnet: write failed: %d\r\n", (int)err);
    }
}

static void telnet_event_handler(telnet_t *session, telnet_event_t *event, void *user_data) {
    (void)user_data;

    switch (event->type) {
    case TELNET_EV_DATA:
        output_enqueue(event->data.buffer, event->data.size);
        break;
    case TELNET_EV_SEND:
        telnet_write_network(event->data.buffer, event->data.size);
        break;
    case TELNET_EV_TTYPE:
        if (event->ttype.cmd == TELNET_TTYPE_SEND) {
            telnet_ttype_is(session, TELNET_TERMINAL_TYPE);
        }
        break;
    case TELNET_EV_DO:
        if (event->neg.telopt == TELNET_TELOPT_NAWS) {
            telnet_send_window_size();
        }
        break;
    case TELNET_EV_WARNING:
        output_printf("\r\ntelnet: warning: %s\r\n", event->error.msg);
        break;
    case TELNET_EV_ERROR:
        output_printf("\r\ntelnet: error: %s\r\n", event->error.msg);
        break;
    default:
        break;
    }
}

static bool telnet_start_protocol(void) {
    telnet_reset();
    telnet = telnet_init(telnet_options, telnet_event_handler, 0, NULL);
    if (!telnet) {
        output_enqueue("telnet: protocol init failed\r\n", 30);
        return false;
    }

    telnet_negotiate(telnet, TELNET_WILL, TELNET_TELOPT_TTYPE);
    telnet_negotiate(telnet, TELNET_WILL, TELNET_TELOPT_NAWS);
    return true;
}

static bool telnet_close_pcb_lwip(struct tcp_pcb *pcb) {
    if (!pcb) {
        return false;
    }
    tcp_arg(pcb, NULL);
    tcp_recv(pcb, NULL);
    tcp_sent(pcb, NULL);
    tcp_err(pcb, NULL);
    if (tcp_close(pcb) != ERR_OK) {
        tcp_abort(pcb);
        return true;
    }
    return false;
}

static err_t telnet_recv_cb(void *arg, struct tcp_pcb *pcb, struct pbuf *p, err_t err) {
    (void)arg;
    if (err != ERR_OK) {
        return err;
    }

    if (!p) {
        telnet_pcb = NULL;
        bool aborted = telnet_close_pcb_lwip(pcb);
        telnet_reset();
        output_enqueue("\r\n[telnet closed]\r\n", 19);
        return aborted ? ERR_ABRT : ERR_OK;
    }

    for (struct pbuf *q = p; q; q = q->next) {
        if (telnet) {
            telnet_recv(telnet, (const char *)q->payload, q->len);
        }
    }
    tcp_recved(pcb, p->tot_len);
    pbuf_free(p);
    return ERR_OK;
}

static void telnet_error(void *arg, err_t err) {
    (void)arg;
    telnet_pcb = NULL;
    telnet_reset();
    output_printf("\r\n[telnet error %d]\r\n", (int)err);
}

static err_t telnet_connected(void *arg, struct tcp_pcb *pcb, err_t err) {
    (void)arg;
    if (err != ERR_OK) {
        telnet_pcb = NULL;
        telnet_reset();
        output_printf("\r\n[telnet connect failed: %d]\r\n", (int)err);
        return err;
    }

    telnet_pcb = pcb;
    if (!telnet_start_protocol()) {
        telnet_pcb = NULL;
        bool aborted = telnet_close_pcb_lwip(pcb);
        return aborted ? ERR_ABRT : ERR_MEM;
    }
    telnet_state = TELNET_CONNECTED;
    output_enqueue("\r\n[telnet connected; ctrl-] closes]\r\n", 36);
    return ERR_OK;
}

static bool telnet_connect_addr_lwip(const ip_addr_t *addr) {
    char ip[48];
    ipaddr_ntoa_r(addr, ip, sizeof(ip));

    struct tcp_pcb *pcb = tcp_new();
    if (!pcb) {
        telnet_reset();
        output_enqueue("telnet: out of tcp pcbs\r\n", 25);
        return false;
    }

    telnet_pcb = pcb;
    telnet_state = TELNET_CONNECTING;
    tcp_arg(pcb, NULL);
    tcp_recv(pcb, telnet_recv_cb);
    tcp_err(pcb, telnet_error);

    err_t err = tcp_connect(pcb, addr, telnet_port, telnet_connected);
    if (err != ERR_OK) {
        tcp_abort(pcb);
        telnet_pcb = NULL;
        telnet_reset();
        output_printf("telnet: connect failed: %d\r\n", (int)err);
        return false;
    }

    output_printf("telnet: connecting %s:%u\r\n", ip, telnet_port);
    return true;
}

static void telnet_dns_found(const char *name, const ip_addr_t *addr, void *arg) {
    uint32_t generation = (uint32_t)(uintptr_t)arg;
    if (generation != telnet_generation || telnet_state != TELNET_RESOLVING) {
        return;
    }
    if (!addr) {
        telnet_reset();
        output_printf("telnet: dns failed for %s\r\n", name ? name : telnet_host);
        return;
    }
    (void)telnet_connect_addr_lwip(addr);
}

void typewriter_net_init(typewriter_net_writer_t writer) {
    output_writer = writer;
    if (wifi_ready) {
        return;
    }

    int ret = cyw43_arch_init();
    if (ret != 0) {
        output_printf("wifi: init failed: %d\r\n", ret);
        return;
    }
    cyw43_arch_enable_sta_mode();
    wifi_ready = true;
    output_enqueue("wifi: ready\r\n", 13);
}

void typewriter_net_task(void) {
    output_drain();

    if (wifi_scan_running && !cyw43_wifi_scan_active(&cyw43_state)) {
        wifi_scan_running = false;
        output_enqueue("scan: done\r\n", 12);
    }

    wifi_poll_connect();
    output_drain();
}

bool typewriter_net_available(void) {
    return wifi_ready;
}

bool typewriter_net_wifi_connected(void) {
    return wifi_link_is_up();
}

bool typewriter_net_wifi_connecting(void) {
    return wifi_connecting;
}

void typewriter_net_scan(void) {
    if (!wifi_ready) {
        output_enqueue("wifi: not ready\r\n", 17);
        return;
    }
    if (wifi_scan_running || cyw43_wifi_scan_active(&cyw43_state)) {
        output_enqueue("scan: already running\r\n", 23);
        return;
    }

    int ret = cyw43_wifi_scan(&cyw43_state, NULL, NULL, wifi_scan_result);
    if (ret != 0) {
        output_printf("scan: failed: %d\r\n", ret);
        return;
    }
    wifi_scan_running = true;
    output_enqueue("scan:\r\n", 7);
}

void typewriter_net_wifi_connect(const char *ssid, const char *password) {
    if (!wifi_ready) {
        output_enqueue("wifi: not ready\r\n", 17);
        return;
    }
    if (!ssid || !ssid[0]) {
        output_enqueue("usage: wifi <ssid> [password]\r\n", 31);
        return;
    }

    const char *key = password && password[0] ? password : NULL;
    uint32_t auth = key ? CYW43_AUTH_WPA2_AES_PSK : CYW43_AUTH_OPEN;
    int ret = cyw43_arch_wifi_connect_async(ssid, key, auth);
    if (ret != 0) {
        output_printf("wifi: connect failed to start: %d\r\n", ret);
        return;
    }

    wifi_connecting = true;
    wifi_last_status = INT_MIN;
    wifi_connect_deadline = make_timeout_time_ms(WIFI_CONNECT_TIMEOUT_MS);
    output_printf("wifi: joining %s\r\n", ssid);
}

void typewriter_net_wifi_status(void) {
    if (!wifi_ready) {
        output_enqueue("wifi: not ready\r\n", 17);
        return;
    }

    int status = cyw43_tcpip_link_status(&cyw43_state, CYW43_ITF_STA);
    output_printf("wifi: %s\r\n", wifi_status_name(status));
    if (status == CYW43_LINK_UP) {
        wifi_print_ip();
    }
}

bool typewriter_net_telnet_active(void) {
    return telnet_state != TELNET_IDLE;
}

bool typewriter_net_telnet_open(const char *host, uint16_t port) {
    if (!wifi_ready) {
        output_enqueue("wifi: not ready\r\n", 17);
        return false;
    }
    if (!wifi_link_is_up()) {
        output_enqueue("wifi: not connected\r\n", 21);
        return false;
    }
    if (!host || !host[0]) {
        output_enqueue("usage: telnet <host> [port]\r\n", 30);
        return false;
    }

    if (telnet_pcb) {
        typewriter_net_telnet_close();
    }

    strncpy(telnet_host, host, sizeof(telnet_host) - 1u);
    telnet_host[sizeof(telnet_host) - 1u] = '\0';
    telnet_port = port ? port : TELNET_DEFAULT_PORT;
    telnet_state = TELNET_RESOLVING;
    uint32_t generation = ++telnet_generation;

    ip_addr_t addr;
    cyw43_arch_lwip_begin();
    bool numeric = ipaddr_aton(telnet_host, &addr);
    err_t err = ERR_OK;
    bool started = false;
    if (numeric) {
        started = telnet_connect_addr_lwip(&addr);
    } else {
        err = dns_gethostbyname(telnet_host, &addr, telnet_dns_found, (void *)(uintptr_t)generation);
        if (err == ERR_OK) {
            started = telnet_connect_addr_lwip(&addr);
        } else if (err == ERR_INPROGRESS) {
            started = true;
            output_printf("telnet: resolving %s\r\n", telnet_host);
        }
    }
    cyw43_arch_lwip_end();

    if (!started) {
        telnet_reset();
        if (err != ERR_OK && err != ERR_INPROGRESS) {
            output_printf("telnet: dns failed: %d\r\n", (int)err);
        }
    }
    return started;
}

void typewriter_net_telnet_send(const char *data, size_t len) {
    if (!telnet || !telnet_pcb || telnet_state != TELNET_CONNECTED || !data || !len) {
        return;
    }

    char buffer[96];
    size_t used = 0;
    for (size_t i = 0; i < len; ++i) {
        char c = data[i];
        if (c == '\r') {
            if (used + 2u > sizeof(buffer)) {
                break;
            }
            buffer[used++] = '\r';
            buffer[used++] = '\n';
        } else {
            if (used + 1u > sizeof(buffer)) {
                break;
            }
            buffer[used++] = c;
        }
    }

    if (!used) {
        return;
    }

    cyw43_arch_lwip_begin();
    telnet_send(telnet, buffer, used);
    cyw43_arch_lwip_end();
}

void typewriter_net_telnet_close(void) {
    cyw43_arch_lwip_begin();
    struct tcp_pcb *pcb = telnet_pcb;
    telnet_pcb = NULL;
    ++telnet_generation;
    (void)telnet_close_pcb_lwip(pcb);
    cyw43_arch_lwip_end();

    if (telnet_state != TELNET_IDLE) {
        output_enqueue("\r\n[telnet closed]\r\n", 19);
    }
    telnet_reset();
}

#else

static typewriter_net_writer_t output_writer;

static void output_text(const char *text) {
    if (output_writer && text) {
        output_writer(text, strlen(text));
    }
}

void typewriter_net_init(typewriter_net_writer_t writer) {
    output_writer = writer;
}

void typewriter_net_task(void) {
}

bool typewriter_net_available(void) {
    return false;
}

void typewriter_net_scan(void) {
    output_text("wifi: build for pico_w\r\n");
}

void typewriter_net_wifi_connect(const char *ssid, const char *password) {
    (void)ssid;
    (void)password;
    output_text("wifi: build for pico_w\r\n");
}

bool typewriter_net_wifi_connected(void) {
    return false;
}

bool typewriter_net_wifi_connecting(void) {
    return false;
}

void typewriter_net_wifi_status(void) {
    output_text("wifi: build for pico_w\r\n");
}

bool typewriter_net_telnet_active(void) {
    return false;
}

bool typewriter_net_telnet_open(const char *host, uint16_t port) {
    (void)host;
    (void)port;
    output_text("telnet: build for pico_w\r\n");
    return false;
}

void typewriter_net_telnet_send(const char *data, size_t len) {
    (void)data;
    (void)len;
}

void typewriter_net_telnet_close(void) {
}

#endif
