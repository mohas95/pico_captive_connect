#include "pico/stdlib.h"
#include "pico/cyw43_arch.h"
#include "lwip/netif.h"
#include "lwip/ip4_addr.h"
#include <cstdio>
#include <cstring>
#include "hardware/watchdog.h"

#include "creds_store.h"
#include "http_portal.h"
#include "dns_hijack.h"
#include "dhcpserver.h"
#include "sta_portal.h"

static dhcp_server_t dhcp;

static void poll_wait() {
#if PICO_CYW43_ARCH_POLL
    cyw43_arch_poll();
    sleep_ms(10);
#else
    sleep_ms(50);
#endif
}


static bool try_sta_connect(const DeviceCreds &c, char *ipbuf, size_t ipbuflen) {
    printf("STA: connecting to '%s'...\n", c.ssid);
    if (cyw43_arch_wifi_connect_timeout_ms(c.ssid, c.wifi_pass, CYW43_AUTH_WPA2_AES_PSK, 20000)) {
        printf("STA: connect failed\n");
        return false;
    }
    auto *netif = netif_list;
    if (netif && netif_is_up(netif)) {
        snprintf(ipbuf, ipbuflen, "%s", ip4addr_ntoa(netif_ip4_addr(netif)));
        printf("STA: connected, IP=%s\n", ipbuf);
        return true;
    }
    printf("STA: no IP (DHCP)\n");
    return false;
}

static void start_softap_and_portal() {
    const char *ap_ssid = "PicoSetup";
    const char *ap_pass = "pico1234";  // WPA2; can be empty for open (not recommended)
    uint32_t channel = 6;

    printf("AP: starting '%s'...\n", ap_ssid);
    cyw43_arch_enable_ap_mode(ap_ssid, ap_pass, CYW43_AUTH_WPA2_AES_PSK);

    // set AP IP = 192.168.4.1 /24
    ip4_addr_t gw, mask;
    IP4_ADDR(&gw, 192,168,4,1);
    IP4_ADDR(&mask, 255,255,255,0);
    // Start DHCP server to hand out 192.168.4.x
    dhcp_server_init(&dhcp, &gw, &mask);
    // DNS hijack -> any hostname -> 192.168.4.1
    dns_hijack_start(gw);
    // HTTP portal
    http_portal_start();

    printf("AP: connect to SSID '%s', password '%s' then open http://setup/ (or any URL)\n", ap_ssid, ap_pass);
}


int main() {
    stdio_init_all();
    sleep_ms(1500);
    printf("\nPico captive portal (poll)\n");

    if (cyw43_arch_init()) {
        printf("CYW43 init failed\n");
        return -1;
    }

    DeviceCreds c{};
    bool have = creds_load(c);

    if (have) {
        cyw43_arch_enable_sta_mode();
        char ip[32];
        if (try_sta_connect(c, ip, sizeof ip)) {
            printf("Ready. (Press reset to re-provision)\n");

            dns_hijack_stop();
            dhcp_server_deinit(&dhcp);
            cyw43_arch_disable_ap_mode();
            
            printf("Web UI available at http://%s\n", ip);
            printf("Netif list IP: %s\n", ip4addr_ntoa(netif_ip4_addr(netif_list)));
            sta_http_start();   // <-- start STA-mode web server
            
            for(;;) poll_wait();
        }
        printf("STA failed; entering provisioning.\n");
    }

    // Provisioning mode
    cyw43_arch_enable_ap_mode("dummy", "", CYW43_AUTH_OPEN); // will be reconfigured
    cyw43_arch_disable_ap_mode(); // ensure clean start
    start_softap_and_portal();

    // Loop until creds saved -> we will just watch flash region periodically
    // (Simple approach: user hits 'Save & Connect', device writes flash, then we reboot.)
    for (;;) {
        DeviceCreds nc{};
        if (creds_load(nc) && (!have || strcmp(nc.ssid, c.ssid) || strcmp(nc.wifi_pass, c.wifi_pass))) {
            printf("New creds saved. Rebooting to connect...\n");
            sleep_ms(500);
            dns_hijack_stop();
            dhcp_server_deinit(&dhcp);
            cyw43_arch_deinit();
            watchdog_reboot(0, 0, 0); // simple reboot
        }
        poll_wait();
    }
}