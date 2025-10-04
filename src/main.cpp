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