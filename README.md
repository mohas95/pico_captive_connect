# Pico Captive Portal and MQTT Client
#### Author: Mohamed Debbagh

This library provides a **captive portal and network provisioning system** for the **Raspberry Pi Pico W / Pico2 W**.  
It allows your project to automatically bring up a Wi-Fi access point for configuration, save credentials in flash,  
connect to a Wi-Fi network (STA mode), and  connect and publish to an MQTT broker.

---

## Features

- **Captive Portal (AP mode)**
  - Starts Pico W as an access point (`SSID: PicoSetup`, password: `pico1234`).
  - Runs a built-in DHCP server and DNS hijack (all requests → setup page).
  - HTTP configuration portal at `http://setup/` (or `192.168.4.1`).

- **STA (Station) Mode**
  - Connects to stored Wi-Fi credentials.
  - Launches a lightweight HTTP server at its assigned IP.
  - Stores MQTT credentials persistently in flash.

- **Automatic Fallback**
  - If STA connection fails, falls back to AP provisioning mode.

- **MQTT Support**
  - Configure MQTT broker/username/password via the STA portal.
  - Connect and publish sensor data.
  - Persistent across reboots.

---
## Requirements 
- Raspberry Pi Pico W / Pico2 W
- Pico SDK
- CMake build system

---

## API Reference

```c
// Initialize networking (STA mode if creds exist, otherwise AP mode)
void net_init();

// Background service (must be called often in main loop)
void net_task();

// Wi-Fi connection state
bool net_is_connected();   // true if Wi-Fi STA connected + IP

// MQTT state
bool mqtt_is_connected();  // true if MQTT session is alive
bool mqtt_connect();       // connect to broker (from saved creds)
void mqtt_try_connect();   // use this one: provides a timeout safe method for connecting to the broker.
bool publish_mqtt(const char* topic, const char* payload, size_t len); // publish to mqtt broker.

// Device identity
const char* net_hostname(); // user-defined or "pico-device"
```
---
## Example usage

To pull this library to your c++ project:

```bash
git clone https://github.com/mohas95/pico_captive_connect.git
```
then in your CMakeLists.txt add: 
```cmake
add_subdirectory(<path to library>/pico_captive_connect)

target_link_libraries(my_project pico_captive_connect)
```

Then in your project code:

``` cpp
#include "pico/stdlib.h"
#include "pico_captive_connect.h"
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include "pico/rand.h"
#include "hardware/watchdog.h"



static absolute_time_t next_pub = 0;

float random_temp(){
    uint32_t r = get_rand_32();
    return 20.0f + (r % 1000) / 100.0f;
}


int main() {
    stdio_init_all();
    sleep_ms(1000);

    if (watchdog_caused_reboot()) {
        printf("[BOOT] System rebooted due to watchdog timeout!\n");
    } else {
        printf("[BOOT] Normal boot or manual reset.\n");
    }

    srand(to_ms_since_boot(get_absolute_time()));
    
    net_init();

    watchdog_enable(30000, 1); 

    while (true) {
        watchdog_update();
        net_task();

        // If Wi-Fi is up but MQTT not yet, keep retrying
        if (net_is_connected() && !mqtt_is_connected()) {
            mqtt_try_connect();
        }

        // Publish only if MQTT connected and it's time
        if (mqtt_is_connected() && absolute_time_diff_us(get_absolute_time(), next_pub) < 0) {
            float temp = random_temp();
            char msg[64];
            snprintf(msg, sizeof(msg), "{\"temp\":%.2f}", temp);

            if (publish_mqtt("sensors/temp", msg, strlen(msg))) {
                printf("[APP] Published temp message: %.2f\n", temp);
                next_pub = make_timeout_time_ms(1000); // normal period
            } else {
                printf("[APP] Publish failed, backing off\n");
                next_pub = make_timeout_time_ms(5000); // backoff if error
            }
        }

        sleep_ms(100);
    }
}
```
---

## User Interface Usage


1. **First boot**  
   - Device starts in **AP mode**.  
   - Connect your phone/PC to Wi-Fi SSID:  
     - **SSID:** `PicoSetup`  
     - **Password:** `pico1234`  
   - Open any browser and go to 192.168.4.1  
   - Enter Wi-Fi credentials.

2. **STA mode**  
   - After reboot, Pico will connect to your Wi-Fi router.  
   - Look in your router’s DHCP table for the assigned IP.

3. **Re-provision**  
   - You can go back to **AP mode** by accessing the configuration page at the assigned IP
   - Press reset if Wi-Fi fails, it falls back to AP mode.  
   - Update credentials via captive portal.

---

## Project Structure

```
pico_captive_connect/
├── include/                       # Public headers (for users to include)
│   ├── creds_store.h              # Flash credential storage API
│   ├── dhcpserver.h               # Lightweight DHCP server
│   ├── dns_hijack.h               # DNS hijack for captive portal redirect
│   ├── http_portal.h              # Captive portal HTTP server
│   ├── lwipopts.h                 # lwIP configuration
│   ├── pico_captive_connect.h     # Main library API (net_init, net_task, MQTT API)
│   └── sta_portal.h               # Web server for STA mode
│
├── src/                           # Implementation files
│   ├── creds_store.cpp
│   ├── dhcpserver.c
│   ├── dns_hijack.cpp
│   ├── http_portal.cpp
│   ├── pico_captive_connect.cpp   # Core library logic
│   ├── sta_portal.cpp
│   └── main.cpp                   # Example app (can be excluded when used as library)
│
├── CMakeLists.txt                 # CMake build setup
├── .gitignore
└── README.md
```
---