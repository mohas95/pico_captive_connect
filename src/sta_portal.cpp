#include "lwip/tcp.h"
#include "creds_store.h"
#include "hardware/watchdog.h"
#include "pico/stdlib.h"
#include <string.h>
#include <stdio.h>
#include "lwip/netif.h"




static struct netif *get_sta_netif() {
    for (struct netif *nif = netif_list; nif; nif = nif->next) {
        const ip4_addr_t *ip = netif_ip4_addr(nif);
        if (!ip4_addr_isany_val(*ip)) {
            printf("Using netif %c%c, IP=%s\n", 
                   nif->name[0], nif->name[1],
                   ip4addr_ntoa(ip));
            return nif;
        }
    }
    return nullptr;
}



static const char *PAGE =
"HTTP/1.1 200 OK\r\nContent-Type: text/html\r\nConnection: close\r\n\r\n"
"<!doctype html><html><body style='font-family:sans-serif'>"
"<h2>Pico Wi-Fi Status</h2>"
"<p>Device is connected to Wi-Fi.</p>"
"<form method='POST' action='/reprovision'>"
"<button type='submit'>Re-Provision</button>"
"</form>"
"</body></html>";

static const char *OK =
"HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nConnection: close\r\n\r\n"
"Rebooting into AP/Provisioning mode...\n";

static err_t on_sent(void *arg, struct tcp_pcb *tpcb, u16_t len) {
    (void)arg; (void)len;
    tcp_close(tpcb);
    return ERR_OK;
}

static err_t on_recv(void *arg, struct tcp_pcb *tpcb, struct pbuf *p, err_t err) {
    (void)arg; (void)err;

    if (!p) {               // remote closed
        tcp_close(tpcb);
        return ERR_OK;
    }

    // Inform lwIP we've received this data
    tcp_recved(tpcb, p->tot_len);

    char req[1024];
    size_t n = pbuf_copy_partial(p, req, sizeof(req)-1, 0);
    req[n] = 0;

    printf("STA HTTP on_recv (len=%u):\n%.*s\n", (unsigned)p->tot_len, (int)n, req);

    if (!strncmp(req, "GET / ", 6)) {
        tcp_write(tpcb, PAGE, strlen(PAGE), TCP_WRITE_FLAG_COPY);
    } else if (!strncmp(req, "POST /reprovision", 17)) {
        WifiCreds empty{}; empty.valid = false;
        creds_save(empty);

        tcp_write(tpcb, OK, strlen(OK), TCP_WRITE_FLAG_COPY);
        tcp_output(tpcb);
        pbuf_free(p);

        sleep_ms(500);
        watchdog_reboot(0, 0, 0);
        return ERR_OK;
    } else {
        tcp_write(tpcb, PAGE, strlen(PAGE), TCP_WRITE_FLAG_COPY);
    }

    tcp_output(tpcb);
    pbuf_free(p);
    return ERR_OK;
}

static err_t on_accept(void *arg, struct tcp_pcb *newpcb, err_t err) {
    (void)arg; (void)err;
    printf("STA HTTP: connection accepted from %s\n", ipaddr_ntoa(&newpcb->remote_ip));
    tcp_recv(newpcb, on_recv);
    tcp_sent(newpcb, on_sent);
    return ERR_OK;
}

void sta_http_start(void) {
    struct netif *nif = get_sta_netif();
    if (!nif) {
        printf("STA HTTP: no STA netif found!\n");
        return;
    }

    if (!netif_is_up(nif)) {
        printf("STA HTTP: netif not UP, forcing up...\n");
        netif_set_up(nif);
    }

    ip4_addr_t ip = *netif_ip4_addr(nif);
    printf("STA HTTP: binding to IP %s\n", ip4addr_ntoa(&ip));

    struct tcp_pcb *pcb = tcp_new_ip_type(IPADDR_TYPE_V4);
    if (!pcb) {
        printf("STA HTTP: tcp_new failed\n");
        return;
    }

    err_t e = tcp_bind(pcb, &ip, 80);
    if (e != ERR_OK) {
        printf("STA HTTP: tcp_bind failed err=%d\n", e);
        tcp_close(pcb);
        return;
    }

    pcb = tcp_listen_with_backlog(pcb, 2);
    tcp_accept(pcb, on_accept);
    printf("STA HTTP server started at %s:80\n", ip4addr_ntoa(&ip));
}