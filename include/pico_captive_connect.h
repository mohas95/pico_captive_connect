#pragma once
#include <stdbool.h>
#include <stddef.h>

// Initialize networking (STA mode if creds exist, otherwise AP portal)
void net_init();

// Background service (must be called often in main loop)
void net_task();

// Query state
bool net_is_connected();   // true if Wi-Fi STA connected + IP
bool mqtt_is_connected();  // true if MQTT session is alive
bool mqtt_connect();
void mqtt_try_connect();
// Publish convenience wrapper
bool publish_mqtt(const char* topic, const char* payload, size_t len);
// Access hostname (stored in creds)
const char* net_hostname();
