#include "lwip/udp.h"
#include "lwip/ip_addr.h"
#include <string.h>

static struct udp_pcb *dns_pcb;
static ip4_addr_t ap_ip;

static void dns_recv(void *arg, struct udp_pcb *upcb, struct pbuf *p, const ip_addr_t *addr, u16_t port) {
    if (!p) return;
    // Minimal parse: echo header, set response bit and one answer, point to our IP
    uint8_t hdr[12];
    if (p->len < 12) { pbuf_free(p); return; }
    pbuf_copy_partial(p, hdr, 12, 0);
    hdr[2] |= 0x80;        // QR=1 (response)
    hdr[3] |= 0x80;        // RA=1
    // set ANCOUNT=1
    hdr[6] = 0; hdr[7] = 1;
    struct pbuf *out = pbuf_alloc(PBUF_TRANSPORT, p->tot_len + 16, PBUF_RAM);
    if (!out) { pbuf_free(p); return; }
    // copy original query
    pbuf_take(out, p->payload, p->tot_len);
    // append minimal answer: pointer to name (0xC0,0x0C), type A(1), class IN(1), TTL 60, RDLEN 4, RDATA ip
    uint8_t ans[16] = {0xC0,0x0C, 0x00,0x01, 0x00,0x01, 0x00,0x00,0x00,0x3C, 0x00,0x04,
                       (uint8_t)ip4_addr1(&ap_ip),(uint8_t)ip4_addr2(&ap_ip),
                       (uint8_t)ip4_addr3(&ap_ip),(uint8_t)ip4_addr4(&ap_ip)};
    pbuf_take_at(out, ans, sizeof(ans), p->tot_len);
    udp_sendto(upcb, out, addr, port);
    pbuf_free(out);
    pbuf_free(p);
}

void dns_hijack_start(ip4_addr_t ip) {
    ap_ip = ip;
    dns_pcb = udp_new();
    udp_bind(dns_pcb, IP_ANY_TYPE, 53);
    udp_recv(dns_pcb, dns_recv, nullptr);
}

void dns_hijack_stop() {
    if (dns_pcb) { udp_remove(dns_pcb); dns_pcb = nullptr; }
}