#include "pico_captive_connect.h"
#include "creds_store.h"
#include "http_portal.h"
#include "dns_hijack.h"
#include "dhcpserver.h"
#include "sta_portal.h"

#include "pico/stdlib.h"
#include "pico/cyw43_arch.h"
#include "lwip/netif.h"
#include "lwip/ip4_addr.h"
#include "lwip/apps/mqtt.h"
#include "lwip/dns.h"
#include <cstdio>
#include <cstring>
#include "hardware/watchdog.h"

static dhcp_server_t dhcp;
static DeviceCreds creds;
static bool connected = false;
static mqtt_client_t* mqtt_client_handle = nullptr;
static absolute_time_t mqtt_connect_next_attempt = 0;


// internal helpers (similar to your current main.cpp)
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

static void start_ap_mode(){
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

static void start_sta_mode(){
    cyw43_arch_enable_sta_mode();
    char ip[32];
    if (try_sta_connect(creds, ip, sizeof ip)) {
        connected = true;

        dns_hijack_stop();
        dhcp_server_deinit(&dhcp);
        cyw43_arch_disable_ap_mode();

        printf("Web UI available at http://%s\n", ip);
        printf("Netif list IP: %s\n", ip4addr_ntoa(netif_ip4_addr(netif_list)));
        sta_http_start();
        return;
    }

    // fallback if STA connect failed
    printf("STA failed; entering provisioning.\n");
    start_ap_mode();
}


bool creds_are_valid(const DeviceCreds &c) {
    if (!c.valid) return false;

    // Require at least SSID and password (or allow open WiFi if you want)
    if (c.ssid[0] == '\0') return false;
    // You might allow empty wifi_pass for open networks, so maybe don’t force that

    return true;
}


bool mqtt_creds_are_valid(const DeviceCreds &c){

    if (c.mqtt_host[0] == '\0') return false;
    if (c.mqtt_port == 0) return false;

    return true;
}


// --- Public API ---

void net_init() {
    printf("\n[pico_captive_connect] init (threadsafe background)\n");

    if (cyw43_arch_init()) {
        printf("CYW43 init failed\n");
        return;
    }

    if (creds_load(creds) && creds_are_valid(creds)) {
        start_sta_mode();
    } else {
        start_ap_mode();
    }
}

void net_task() {
    if (!connected) {
        // check if new creds got saved while in AP mode
        DeviceCreds nc{};
        if (creds_load(nc) && (strcmp(nc.ssid, creds.ssid) || strcmp(nc.wifi_pass, creds.wifi_pass))) {
            printf("New creds saved. Rebooting to connect...\n");
            creds=nc;
            sleep_ms(500);
            dns_hijack_stop();
            dhcp_server_deinit(&dhcp);
            cyw43_arch_deinit();
            watchdog_reboot(0, 0, 0);
        }
    }

    // if using lwIP MQTT, we could pump keepalives here if needed
    if (connected && mqtt_client_handle) {
        // mqtt_client_poll(mqtt_client_handle);  // if required
    }

    tight_loop_contents(); // yield
}


bool net_is_connected() { return connected; }

static void mqtt_connection_cb(mqtt_client_t *client, void *arg, mqtt_connection_status_t status){
    if (status == MQTT_CONNECT_ACCEPTED){
        printf("[MQTT] Connected!\n");
    } else{
        printf("[MQTT] Connection failed, status=%d!\n", status);
        mqtt_client_handle = nullptr;    
    }
}

bool mqtt_connect(){

    if (!mqtt_creds_are_valid(creds)) {
        printf("[MQTT] Skipping connect: no broker configured.\n");
        return false;
    }

    if(!net_is_connected()){
        printf("[MQTT] WIFI  is not connected. \n");
        return false;
    }

    if (mqtt_client_handle){
        printf("[MQTT] Already Connected \n"); 
        return true;
    }

    ip_addr_t broker_ip;
    err_t err = dns_gethostbyname(creds.mqtt_host, &broker_ip, NULL, NULL);
    if(err == ERR_INPROGRESS){
        printf("[MQTT] Resolving hostname ...\n");

        return false;
    } else if (err != ERR_OK) {
        printf("[MQTT] DNS lookup failed for %s\n", creds.mqtt_host);
        return false;
    }

    mqtt_client_handle = mqtt_client_new();
    if(!mqtt_client_handle){
        printf("[MQTT] Failed to allocate client. \n");
        return false;
    }

    mqtt_connect_client_info_t ci{};
    ci.client_id = creds.hostname[0] ? creds.hostname : "pico-client";
    ci.client_user = creds.mqtt_user[0] ? creds.mqtt_user : NULL;
    ci.client_pass = creds.mqtt_pass[0] ? creds.mqtt_pass : NULL;

    err = mqtt_client_connect(mqtt_client_handle, &broker_ip, creds.mqtt_port ? creds.mqtt_port : 1883, mqtt_connection_cb, NULL, &ci);

    if (err != ERR_OK){
        printf("[MQTT] Connect failed err=%d\n", err);
        mqtt_client_handle = nullptr;
        return false;
    }

    printf("[MQTT] Connecting to %s:%d... \n", creds.mqtt_host, creds.mqtt_port);
    return true;
}


void mqtt_try_connect() {
    if (absolute_time_diff_us(get_absolute_time(), mqtt_connect_next_attempt) > 0) {
        return; // wait until timeout
    }
    if (!mqtt_connect()) {
        // failed, try again in 5 seconds
        mqtt_connect_next_attempt = make_timeout_time_ms(5000);
    }
}


bool mqtt_is_connected() {
    return connected && mqtt_client_handle;
}

bool publish_mqtt(const char* topic, const char* payload, size_t len) {
    if (!mqtt_is_connected()) return false;
    err_t err = mqtt_publish(mqtt_client_handle, topic, payload, len,
                             0, 0, NULL, NULL);
    if (err != ERR_OK) {
        printf("[MQTT] publish failed, err=%d\n", err);
        return false;
    }
    return true;
}

const char* net_hostname() {
    return creds.hostname[0] ? creds.hostname : "pico-device";
}
