# Pico Web Captive Portal

This project turns a **Raspberry Pi Pico W / Pico2 W** into a **Wi-Fi access point with captive portal** that can also connect to an existing Wi-Fi network (STA mode). It provides an HTTP setup portal, DHCP/DNS services, and optional mDNS support for easy device discovery.

---

## Features

- **Captive Portal (AP mode)**  
  - Starts as a Wi-Fi access point (`PicoSetup` / password: `pico1234`).  
  - Runs a built-in DHCP server and DNS hijack so any URL resolves to the setup page.  
  - Hosts a provisioning HTTP portal (`http://setup/`) to configure Wi-Fi credentials.

- **Wi-Fi Client (STA mode)**  
  - Connects to a saved SSID and password.  
  - If successful, starts the web server on the assigned IP.  
  - Stores Wi-Fi credentials in flash (persistent across reboots).

- **Automatic Fallback**  
  - If STA connection fails, falls back to AP + captive portal mode for re-provisioning.

- **Optional mDNS support** (if your SDK build includes `LWIP_MDNS_RESPONDER`)  
  - Can advertise as `http://pico.local`.  
  - Provides service discovery (`_http._tcp`).

---

## Project Structure

```
src/
 ├── main.cpp         # Main firmware logic
 ├── creds_store.h/.c # Store/load Wi-Fi credentials in flash
 ├── http_portal.*    # Simple captive portal HTTP server
 ├── dns_hijack.*     # DNS hijack for redirecting requests
 ├── dhcpserver.*     # Lightweight DHCP server
 ├── sta_portal.*     # Web server for STA mode
CMakeLists.txt
README.md

```

---

## Usage


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
