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
"<h2>Pico Device Configuration</h2>"
"<p>Device is connected to Wi-Fi.</p>"
"<form method='POST' action='/save_mqtt'>"
"MQTT Host:<br><input name='h' maxlength='63'><br>"
"Port:<br><input name='o' maxlength='5'><br>"
"Username:<br><input name='u' maxlength='31'><br>"
"Password:<br><input name='w' type='password' maxlength='31'><br><br>"
"Device Hostname:<br><input name='n' maxlength='31'><br><br>"
"<button type='submit'>Save & Reboot</button>"
"</form><br>"
"<form method='POST' action='/reprovision'>"
"<button type='submit'>Re-Provision Wi-Fi</button>"
"</form>"
"</body></html>";


static void send_config_page(struct tcp_pcb *tpcb) {
    DeviceCreds c{};
    creds_load(c);  // load saved creds (if any)

    char page[1024];
    snprintf(page, sizeof(page),
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: text/html\r\n"
        "Connection: close\r\n\r\n"
        "<!doctype html><html><body style='font-family:sans-serif'>"
        "<h2>Pico Device Configuration</h2>"
        "<p>Device is connected to Wi-Fi.</p>"
        "<form method='POST' action='/save_mqtt'>"
        "MQTT Host:<br><input name='h' maxlength='63' value='%s'><br>"
        "Port:<br><input name='o' maxlength='5' value='%d'><br>"
        "Username:<br><input name='u' maxlength='31' value='%s'><br>"
        "Password:<br><input name='w' type='password' maxlength='31' value=''><br><br>"
        "Device Hostname:<br><input name='n' maxlength='31' value='%s'><br><br>"
        "<button type='submit'>Save & Reboot</button>"
        "</form><br>"
        "<form method='POST' action='/reprovision'>"
        "<button type='submit'>Re-Provision Wi-Fi</button>"
        "</form>"
        "</body></html>",
        c.mqtt_host[0] ? c.mqtt_host : "",
        c.mqtt_port ? c.mqtt_port : 1883,   // default 1883 if none saved
        c.mqtt_user[0] ? c.mqtt_user : "",
        c.hostname[0] ? c.hostname : "pico-device"
    );

    tcp_write(tpcb, page, strlen(page), TCP_WRITE_FLAG_COPY);
}



static const char *OK =
"HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nConnection: close\r\n\r\n"
"Rebooting into AP/Provisioning mode...\n";

static void url_decode(char *s) {
    // very small decoder for %XX and + -> space
    char *r=s, *w=s;
    while (*r) {
        if (*r=='%') { int v; if (sscanf(r+1,"%2x",&v)==1){ *w++=(char)v; r+=3; } else { *w++=*r++; } }
        else if (*r=='+') { *w++=' '; r++; }
        else { *w++=*r++; }
    }
    *w=0;
}

static void parse_and_save_mqtt(const char *body, size_t len){

    char buf[300]; if (len >=sizeof(buf)) len = sizeof(buf)-1;

    memcpy(buf,body,len); buf[len]=0;

    DeviceCreds c{};
    if (!creds_load(c)) {
        memset(&c,0, sizeof(c));
    }

    char *tok =strtok(buf, "&");
    while (tok){
        if (!strncmp(tok,"h=",2)) { strncpy(c.mqtt_host, tok+2, sizeof(c.mqtt_host)-1); url_decode(c.mqtt_host); }
        else if (!strncmp(tok,"o=",2)) { c.mqtt_port = atoi(tok+2); }
        else if (!strncmp(tok,"u=",2)) { strncpy(c.mqtt_user, tok+2, sizeof(c.mqtt_user)-1); url_decode(c.mqtt_user); }
        else if (!strncmp(tok,"w=",2)) { strncpy(c.mqtt_pass, tok+2, sizeof(c.mqtt_pass)-1); url_decode(c.mqtt_pass); }
        else if (!strncmp(tok,"n=",2)) { strncpy(c.hostname, tok+2, sizeof(c.hostname)-1); url_decode(c.hostname); }
        tok = strtok(nullptr, "&");
    }

    c.valid = true;
    printf("Saving MQTT creds: HOST='%s', PORT=%d, USER='%s', Device Hostname='%s'\n", c.mqtt_host, c.mqtt_port, c.mqtt_user, c.hostname);
    creds_save(c);

}


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
        // tcp_write(tpcb, PAGE, strlen(PAGE), TCP_WRITE_FLAG_COPY);
        send_config_page(tpcb);
    } else if (!strncmp(req, "POST /save_mqtt", 15)) {
        const char *body = strstr(req, "\r\n\r\n");
        if (body) {
            body += 4;
            parse_and_save_mqtt(body, strlen(body));
        }
        tcp_write(tpcb, OK, strlen(OK), TCP_WRITE_FLAG_COPY);
        tcp_output(tpcb);
        pbuf_free(p);

        sleep_ms(500);
        watchdog_reboot(0, 0, 0);
        return ERR_OK;
    } else if (!strncmp(req, "POST /reprovision", 17)) {
        DeviceCreds empty{}; empty.valid = false;
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