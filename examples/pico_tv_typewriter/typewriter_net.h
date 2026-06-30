#ifndef TYPEWRITER_NET_H
#define TYPEWRITER_NET_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef void (*typewriter_net_writer_t)(const char *data, size_t len);

void typewriter_net_init(typewriter_net_writer_t writer);
void typewriter_net_task(void);
bool typewriter_net_available(void);
void typewriter_net_scan(void);
void typewriter_net_wifi_connect(const char *ssid, const char *password);
bool typewriter_net_wifi_connected(void);
bool typewriter_net_wifi_connecting(void);
void typewriter_net_wifi_status(void);
bool typewriter_net_telnet_active(void);
bool typewriter_net_telnet_open(const char *host, uint16_t port);
void typewriter_net_telnet_send(const char *data, size_t len);
void typewriter_net_telnet_close(void);

#endif
