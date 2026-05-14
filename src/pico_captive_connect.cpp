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
#include "lwip/timeouts.h"
// #include "lwip/tcp.h"
#include <cstdio>
#include <cstring>
#include "hardware/watchdog.h"

static dhcp_server_t dhcp;
static DeviceCreds creds;
static bool connected = false;
static mqtt_client_t* mqtt_client_handle = nullptr;
static absolute_time_t mqtt_connect_next_attempt = 0;
static bool in_ap_mode = false;
// static bool mqtt_inflight = false;

static mqtt_message_callback_t user_msg_cb = nullptr;
static char incoming_topic[128];
static char incoming_payload[512];
static size_t incoming_len = 0;

static absolute_time_t next_check = 0;
static absolute_time_t next_sta_retry = 0;
static int lost_counter = 0;


// New state machine for MQTT
enum MqttState {
    MQTT_DISCONNECTED,
    MQTT_CONNECTING,
    MQTT_CONNECTED
};
static MqttState mqtt_state = MQTT_DISCONNECTED;

// ------------------- Wi-Fi Helpers -------------------

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

static void net_stop_all() {
    printf("[NET] Stopping all network services...\n");
    dns_hijack_stop();
    dhcp_server_deinit(&dhcp);
    cyw43_arch_disable_ap_mode();
    cyw43_wifi_leave(&cyw43_state, CYW43_ITF_STA);    
    connected = false;
}


static void start_ap_mode(){
    net_stop_all();

    in_ap_mode = true;
    connected = false;
    const char *ap_ssid = "PicoSetup";
    const char *ap_pass = "pico1234";
    uint32_t channel = 6;

    printf("AP: starting '%s'...\n", ap_ssid);
    cyw43_arch_enable_ap_mode(ap_ssid, ap_pass, CYW43_AUTH_WPA2_AES_PSK);

    ip4_addr_t gw, mask;
    IP4_ADDR(&gw, 192,168,4,1);
    IP4_ADDR(&mask, 255,255,255,0);
    dhcp_server_init(&dhcp, &gw, &mask);
    dns_hijack_start(gw);
    http_portal_start();

    printf("AP: connect to SSID '%s', password '%s' then open http://setup/\n", ap_ssid, ap_pass);
    next_sta_retry = make_timeout_time_ms(300000); 

}

static void start_sta_mode(){
    net_stop_all();

    in_ap_mode = false;
    cyw43_arch_enable_sta_mode();
    char ip[32];
    if (try_sta_connect(creds, ip, sizeof ip)) {
        connected = true;
        dns_hijack_stop();
        dhcp_server_deinit(&dhcp);
        cyw43_arch_disable_ap_mode();

        printf("Web UI available at http://%s\n", ip);
        sta_http_start();
        return;
    }
    connected = false;
    printf("STA failed; entering provisioning.\n");
    start_ap_mode();
}

// ------------------- Credential Checks -------------------

bool creds_are_valid(const DeviceCreds &c) {
    if (!c.valid) return false;
    if (c.ssid[0] == '\0') return false;
    return true;
}

bool mqtt_creds_are_valid(const DeviceCreds &c){
    if (c.mqtt_host[0] == '\0') return false;
    if (c.mqtt_port == 0) return false;
    return true;
}

// ------------------- Public API -------------------

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
    if (!net_is_connected()) {
        DeviceCreds nc{};
        // if (creds_load(nc) && memcmp(&nc, &creds, sizeof(DeviceCreds)) != 0) {
        if (creds_load(nc) && nc.dirty) {
            printf("New creds saved. Rebooting to connect...\n");
            creds=nc;
            sleep_ms(500);
            dns_hijack_stop();
            dhcp_server_deinit(&dhcp);
            cyw43_arch_deinit();
            watchdog_reboot(0, 0, 0);
        }
    }

    sys_check_timeouts();
    tight_loop_contents();

    // reboot if Wi-Fi disconnects unexpectedly
    if (absolute_time_diff_us(get_absolute_time(), next_check) < 0) {
        next_check = make_timeout_time_ms(5000);

        if (!in_ap_mode) {
            int status = cyw43_wifi_link_status(&cyw43_state, CYW43_ITF_STA);

            // Only count as "lost" if fully disconnected or auth failed
            bool truly_down = (
                status == CYW43_LINK_DOWN ||
                status == CYW43_LINK_BADAUTH ||
                status == CYW43_LINK_NONET
            );

            if (truly_down) {
                if (++lost_counter > 6) {   // e.g. 6×5 s = 30 s of real loss
                    printf("[NET] Wi-Fi link lost (status=%d, counter=%d). Rebooting...\n", status, lost_counter);
                    dns_hijack_stop();
                    dhcp_server_deinit(&dhcp);
                    cyw43_arch_deinit();
                    watchdog_reboot(0, 0, 0);
                }
            } else {
                lost_counter = 0;  // reset whenever link is OK
            }
        }
    }

    // Periodically try to reconnect to stored STA credentials if in AP mode

    if (in_ap_mode && absolute_time_diff_us(get_absolute_time(), next_sta_retry) < 0) {
        next_sta_retry = make_timeout_time_ms(300000); // retry every 5 minutes

        DeviceCreds stored{};
        if (creds_load(stored) && creds_are_valid(stored)) {
            printf("[NET] Saved Wi-Fi credentials found — rebooting to retry STA connection...\n");

            // Gracefully stop network services before reboot
            dns_hijack_stop();
            dhcp_server_deinit(&dhcp);
            cyw43_arch_deinit();

            sleep_ms(100);
            watchdog_reboot(0, 0, 0);
        } else {
            printf("[NET] No valid credentials found — staying in AP mode.\n");
        }
    }

}

bool net_is_connected() {
    if (in_ap_mode) return false; // never "connected" in AP mode
    auto *netif = netif_list;
    return netif && netif_is_up(netif) && connected;

}

// ------------------- MQTT -------------------

static void mqtt_reset_client() {
    mqtt_state = MQTT_DISCONNECTED;

    if (mqtt_client_handle) {
        mqtt_client_free(mqtt_client_handle);
        mqtt_client_handle = nullptr;
    }

}

static void mqtt_incoming_publish_cb(
    void *arg,
    const char *topic,
    u32_t tot_len
) {
    snprintf(incoming_topic, sizeof(incoming_topic), "%s", topic);
    incoming_len = 0;

    if (tot_len >= sizeof(incoming_payload)) {
        printf("[MQTT] Incoming payload too large: %lu\n", (unsigned long)tot_len);
    }
}

static void mqtt_incoming_data_cb(
    void *arg,
    const u8_t *data,
    u16_t len,
    u8_t flags
) {
    size_t space_left = sizeof(incoming_payload) - 1 - incoming_len;

    if (len > space_left) {
        len = space_left;
    }

    memcpy(&incoming_payload[incoming_len], data, len);
    incoming_len += len;
    incoming_payload[incoming_len] = '\0';

    if (flags & MQTT_DATA_FLAG_LAST) {
        if (user_msg_cb) {
            user_msg_cb(incoming_topic, incoming_payload, incoming_len);
        }

        incoming_len = 0;
    }
}

static void mqtt_connection_cb(mqtt_client_t *client, void *arg, mqtt_connection_status_t status){
    if (status == MQTT_CONNECT_ACCEPTED){
        printf("[MQTT] Connected!\n");
        // mqtt_inflight = false; 
        mqtt_state = MQTT_CONNECTED;

        mqtt_set_inpub_callback(
            client,
            mqtt_incoming_publish_cb,
            mqtt_incoming_data_cb,
            NULL
        );

    } else {
        printf("[MQTT] Connection failed, status=%d!\n", status);
        mqtt_reset_client();
        // mqtt_inflight = false; 
    }
}

static void mqtt_pub_cb(void *arg, err_t result) {
    // mqtt_inflight = false; 
    if (result == ERR_OK) {
        // printf("[MQTT] Publish confirmed\n");
    } else {
        printf("[MQTT] Publish failed with err=%d\n", result);
        // mqtt_reset_client();
    }
}

static void mqtt_sub_cb(void *arg, err_t result) {
    if (result == ERR_OK) {
        printf("[MQTT] Subscribe successful\n");
    } else {
        printf("[MQTT] Subscribe failed err=%d\n", result);
    }
}


bool mqtt_connect(){
    if (!mqtt_creds_are_valid(creds)) {
        printf("[MQTT] Skipping connect: no broker configured.\n");
        return false;
    }
    if(!net_is_connected()){
        printf("[MQTT] WIFI is not connected.\n");
        return false;
    }
    if (mqtt_state == MQTT_CONNECTED) {
        return true;
    }
    if (mqtt_state == MQTT_CONNECTING) {
        return false; // already in progress
    }

    if (mqtt_client_handle) {
        mqtt_client_free(mqtt_client_handle);
        mqtt_client_handle = nullptr;
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
        printf("[MQTT] Failed to allocate client.\n");
        return false;
    }

    mqtt_connect_client_info_t ci{};
    ci.client_id = creds.hostname[0] ? creds.hostname : "pico-client";
    ci.client_user = creds.mqtt_user[0] ? creds.mqtt_user : NULL;
    ci.client_pass = creds.mqtt_pass[0] ? creds.mqtt_pass : NULL;
    ci.keep_alive = 60;

    err = mqtt_client_connect(mqtt_client_handle, &broker_ip,
                              creds.mqtt_port ? creds.mqtt_port : 1883,
                              mqtt_connection_cb, NULL, &ci);
    if (err != ERR_OK){
        printf("[MQTT] Connect failed err=%d\n", err);
        mqtt_reset_client();
        return false;
    }

    printf("[MQTT] Connecting to %s:%d...\n", creds.mqtt_host, creds.mqtt_port);
    mqtt_state = MQTT_CONNECTING;
    return false;
}

void mqtt_try_connect() {
    if (absolute_time_diff_us(get_absolute_time(), mqtt_connect_next_attempt) > 0) {
        return;
    }
    if (mqtt_state == MQTT_DISCONNECTED) {
        if (!mqtt_connect()) {
            mqtt_connect_next_attempt = make_timeout_time_ms(5000);
        }
    }
}

bool mqtt_is_connected() {
    // return (mqtt_state == MQTT_CONNECTED);
    return mqtt_state == MQTT_CONNECTED &&
           mqtt_client_handle &&
           mqtt_client_is_connected(mqtt_client_handle);
}


bool subscribe_mqtt(const char* topic, mqtt_message_callback_t cb) {
    if (!mqtt_is_connected()) {
        printf("[MQTT] Cannot subscribe: not connected\n");
        return false;
    }

    user_msg_cb = cb;

    err_t err = mqtt_subscribe(
        mqtt_client_handle,
        topic,
        0,
        mqtt_sub_cb,
        NULL
    );

    if (err != ERR_OK) {
        printf("[MQTT] Subscribe request failed err=%d\n", err);
        return false;
    }

    printf("[MQTT] Subscribing to %s\n", topic);
    return true;
}



bool publish_mqtt(const char* topic, const char* payload, size_t len) {
    if (!mqtt_is_connected()) return false;
    // if (mqtt_inflight) return false;

    err_t err = mqtt_publish(mqtt_client_handle, topic, payload, len, 0, 0, mqtt_pub_cb, NULL);
    if (err == ERR_MEM) {
        // lwIP queue full, don't panic, just retry later
        printf("[MQTT] ERR_MEM (queue full), will retry\n");
        return false;
    }
    if (err != ERR_OK) {
        printf("[MQTT] publish failed, err=%d\n", err);
        
        // mqtt_inflight = false;
        mqtt_reset_client();
        return false;
        
    }

    // mqtt_inflight = true;
    return true;
}

const char* net_hostname() {
    return creds.hostname[0] ? creds.hostname : "pico-device";
}
