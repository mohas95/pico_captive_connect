#include "pico/stdlib.h"
#include "pico_captive_connect.h"
#include <cstring>
#include <cstdio>

static absolute_time_t next_pub = 0;


int main() {
    stdio_init_all();
    sleep_ms(1000);

    net_init();

    while (true) {
        net_task();

        // If Wi-Fi is up but MQTT not yet, keep retrying
        if (net_is_connected() && !mqtt_is_connected()) {
            mqtt_try_connect();
        }

        // Publish only if MQTT connected and it's time
        if (mqtt_is_connected() && absolute_time_diff_us(get_absolute_time(), next_pub) < 0) {
            const char* msg = "{\"temp\":23.5}";
            if (publish_mqtt("sensors/temp", msg, strlen(msg))) {
                printf("[APP] Published temp message\n");
                next_pub = make_timeout_time_ms(1000); // normal period
            } else {
                printf("[APP] Publish failed, backing off\n");
                next_pub = make_timeout_time_ms(5000); // backoff if error
            }
        }

        // Let background tasks run frequently
        sleep_ms(10);
    }
}