#include "pico/stdlib.h"
#include "pico_captive_connect.h"
#include <cstring>

int main(){

    stdio_init_all();
    sleep_ms(1000);
    
    net_init();

    while (true){
        net_task();

        if(mqtt_is_connected()) {
            const char* msg = "{\"temp\":23.5}";
            publish_mqtt("sensors/temp", msg, strlen(msg));
        }
        sleep_ms(1000);
    }
}