#pragma once
#include <stdbool.h>

struct WifiCreds{
    char ssid[33];  //32+NUL
    char pass[65]; //64+ NUL
    bool valid;
};


bool creds_load(WifiCreds &out);
bool creds_save(const WifiCreds &in);
void creds_clear();