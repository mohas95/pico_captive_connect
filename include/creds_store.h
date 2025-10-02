#pragma once
#include <stdbool.h>
#include <stdint.h>

// struct WifiCreds{
//     char ssid[33];  //32+NUL
//     char pass[65]; //64+ NUL
//     bool valid;
// };


// bool creds_load(WifiCreds &out);
// bool creds_save(const WifiCreds &in);
// void creds_clear();


struct DeviceCreds{
    bool valid;
    
    // wifi creds
    char ssid[33];  //32+NUL
    char wifi_pass[65]; //64+ NUL
    
    // mqtt creds
    char mqtt_host[64];
    uint16_t mqtt_port;
    char mqtt_user[32];
    char mqtt_pass[64];
    char mqtt_topic[64];

    // device identity
    char hostname[32];

};

bool creds_load(DeviceCreds &out);
bool creds_save(const DeviceCreds &in);
void creds_clear();