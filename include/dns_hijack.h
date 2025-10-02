#pragma once
#include "lwip/ip4_addr.h"

void dns_hijack_start(ip4_addr_t ap_ip);
void dns_hijack_stop();
