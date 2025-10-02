#include "lwip/tcp.h"
#include <string.h>
#include <stdio.h>
#include "creds_store.h"

static const char *PAGE =
"HTTP/1.1 200 OK\r\nContent-Type: text/html\r\nConnection: close\r\n\r\n"
"<!doctype html><html><body style='font-family:sans-serif'>"
"<h2>Pico Wi-Fi Setup</h2>"
"<form method='POST' action='/save'>"
"SSID:<br><input name='s' maxlength='32'><br>"
"Password:<br><input name='p' type='password' maxlength='64'><br><br>"
"Device Hostname:<br><input name='n' maxlength='31'><br><br>"
"<button type='submit'>Save & Connect</button>"
"</form></body></html>";

static const char *OK =
"HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nConnection: close\r\n\r\nSaved. Connecting…\n";

static err_t on_sent(void *arg, struct tcp_pcb *tpcb, u16_t len) { (void)arg;(void)len; tcp_close(tpcb); return ERR_OK; }

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

static void parse_and_save(const char *body, size_t len) {
    // parse s=...&p=...
    char buf[200]; if (len >= sizeof(buf)) len = sizeof(buf)-1;
    memcpy(buf, body, len); buf[len]=0;
    DeviceCreds c{}; c.valid=false;
    char *tok = strtok(buf, "&");
    while (tok) {
        if (!strncmp(tok,"s=",2)) { strncpy(c.ssid, tok+2, sizeof(c.ssid)-1); url_decode(c.ssid); }
        else if (!strncmp(tok,"p=",2)) { strncpy(c.wifi_pass, tok+2, sizeof(c.wifi_pass)-1); url_decode(c.wifi_pass); }
        else if (!strncmp(tok,"n=",2)) { strncpy(c.hostname, tok+2, sizeof(c.hostname)-1); url_decode(c.hostname); }
        tok = strtok(nullptr, "&");
    }
    if (c.ssid[0]) { 
        c.valid=true; 
        
        // Mask the password with '*' but keep the same length
        char masked_pass[65];
        size_t pass_len = strlen(c.wifi_pass);
        if (pass_len >= sizeof(masked_pass)) pass_len = sizeof(masked_pass) - 1;
        memset(masked_pass, '*', pass_len);
        masked_pass[pass_len] = '\0';

        printf("Saving creds: SSID='%s', PASS='%s', Device Hostname='%s'\n", c.ssid, masked_pass, c.hostname); // <-- debug
        // printf("Saving creds: SSID='%s', PASS='%s'\n", c.ssid, c.wifi_pass); // <-- debug
        creds_save(c); 
    }

}

static err_t on_recv(void *arg, struct tcp_pcb *tpcb, struct pbuf *p, err_t err) {
    if (!p) { tcp_close(tpcb); return ERR_OK; }
    char req[1024]; size_t n = pbuf_copy_partial(p, req, sizeof(req)-1, 0); req[n]=0;
    printf("HTTP request:\n%s\n", req); // <-- dump the full request

    if (!strncmp(req, "GET / ", 6)) {
        tcp_write(tpcb, PAGE, strlen(PAGE), TCP_WRITE_FLAG_COPY);
    } else if (!strncmp(req, "POST /save", 10)) {
        // find body
        const char *body = strstr(req, "\r\n\r\n");
        if (body) {
            body += 4;
            parse_and_save(body, strlen(body));
        }
        tcp_write(tpcb, OK, strlen(OK), TCP_WRITE_FLAG_COPY);
    } else {
        tcp_write(tpcb, PAGE, strlen(PAGE), TCP_WRITE_FLAG_COPY);
    }
    tcp_output(tpcb);
    pbuf_free(p);
    return ERR_OK;
}

static err_t on_accept(void *arg, struct tcp_pcb *newpcb, err_t err) {
    tcp_recv(newpcb, on_recv);
    tcp_sent(newpcb, on_sent);
    return ERR_OK;
}

void http_portal_start() {
    struct tcp_pcb *pcb = tcp_new_ip_type(IPADDR_TYPE_ANY);
    tcp_bind(pcb, IP_ANY_TYPE, 80);
    pcb = tcp_listen_with_backlog(pcb, 2);
    tcp_accept(pcb, on_accept);
}
