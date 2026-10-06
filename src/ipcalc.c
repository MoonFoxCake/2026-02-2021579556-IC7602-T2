/*
 * ipcalc.c - Calculadora de subredes IPv4 (Persona 3)
 *
 * Implementa el contrato de ipcalc.h: cada función recibe los argumentos como
 * cadenas, los valida y escribe SOLO el valor en 'out'. No imprime nada y no
 * usa variables globales mutables, así que es segura con hilos.
 *
 * Todas las direcciones se manejan como uint32_t en orden de host, para poder
 * operar con bitwise: red = ip & mask, broadcast = red | ~mask.
 */
#include <ctype.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ipcalc.h"

/* Escribe con formato en out; RC_ERR_INTERNAL si no cabe. */
static int put(char *out, size_t n, const char *fmt, ...)
{
    if (!out || n == 0) return RC_ERR_INTERNAL;
    va_list ap;
    va_start(ap, fmt);
    int w = vsnprintf(out, n, fmt, ap);
    va_end(ap);
    return (w < 0 || (size_t)w >= n) ? RC_ERR_INTERNAL : RC_OK;
}

// Pasa la IP (uint32_t) a texto X.X.X.X
void ipc_ip_to_str(uint32_t ip, char *buf, size_t n)
{
    snprintf(buf, n, "%u.%u.%u.%u",
             (ip >> 24) & 0xFFu,
             (ip >> 16) & 0xFFu,
             (ip >>  8) & 0xFFu,
              ip        & 0xFFu);
}

// Se fija que la IP sea valida, y si es asi, la pasa a uint32_t
int ipc_parse_ip(const char *str, uint32_t *out)
{
    unsigned int a, b, c, d;
    char extra;
    if (!str || !*str) return 0;
    // %c detecta basura al final (ej. "1.2.3.4abc")
    if (sscanf(str, "%u.%u.%u.%u%c", &a, &b, &c, &d, &extra) != 4) return 0;
    //Revisa que ninguno se pase de 255
    if (a > 255u || b > 255u || c > 255u || d > 255u) return 0;
    *out = ((uint32_t)a << 24) | ((uint32_t)b << 16) |
           ((uint32_t)c <<  8) |  (uint32_t)d;
    return 1;
}

// Recibe la mascara (/N o X.X.X.X); devuelve el prefijo o -1 si es invalida
int ipc_parse_mask(const char *str, uint32_t *mask_out)
{
    if (!str || !*str) return -1;

    // Formato /N: solo digitos despues del /
    if (str[0] == '/') {
        const char *p = str + 1;
        if (!*p) return -1;
        for (const char *q = p; *q; q++)
            if (!isdigit((unsigned char)*q)) return -1;
        char *endp = NULL;
        long v = strtol(p, &endp, 10);
        if (*endp != '\0' || v < 0 || v > 32) return -1;
        // Lo convierte a bits (desplazar 32 posiciones no esta definido en C)
        if (mask_out) *mask_out = (v == 0) ? 0u : 0xFFFFFFFFu << (32 - (int)v);
        return (int)v;
    }

    // Formato X.X.X.X
    uint32_t m;
    if (!ipc_parse_ip(str, &m)) return -1;

    //Cuenta cuantos 1 hay; un 1 despues de un 0 hace la mascara invalida
    int prefix = 0;
    int found_zero = 0;
    for (int i = 31; i >= 0; i--) {
        if (m & (1u << i)) {
            if (found_zero) return -1;
            prefix++;
        } else {
            found_zero = 1;
        }
    }
    if (mask_out) *mask_out = m;
    return prefix;
}

/* Valida IP y máscara a la vez; RC_OK o el código de error que corresponda. */
static int parse_ip_mask(const char *ip_s, const char *mask_s,
                         uint32_t *ip, uint32_t *mask, int *prefix)
{
    if (!ipc_parse_ip(ip_s, ip)) return RC_ERR_IP;
    int p = ipc_parse_mask(mask_s, mask);
    if (p < 0) return RC_ERR_MASK;
    if (prefix) *prefix = p;
    return RC_OK;
}

/* 1. Broadcast = red con todos los bits de host en 1 */
int ipc_broadcast(const char *ip_s, const char *mask_s, char *out, size_t n)
{
    uint32_t ip, mask;
    int rc = parse_ip_mask(ip_s, mask_s, &ip, &mask, NULL);
    if (rc != RC_OK) return rc;
    char buf[16];
    ipc_ip_to_str((ip & mask) | ~mask, buf, sizeof buf);
    return put(out, n, "%s", buf);
}

/* 2. Número de red = bits de host en 0 */
int ipc_network(const char *ip_s, const char *mask_s, char *out, size_t n)
{
    uint32_t ip, mask;
    int rc = parse_ip_mask(ip_s, mask_s, &ip, &mask, NULL);
    if (rc != RC_OK) return rc;
    char buf[16];
    ipc_ip_to_str(ip & mask, buf, sizeof buf);
    return put(out, n, "%s", buf);
}

/*
 * 3. Rango de hosts. Formato del enunciado: los octetos comunes al primer y
 * último host, y lo que cambia entre llaves.
 *     /29 -> 10.8.2.{1-6}      /16 -> 172.16.{0.1-255.254}
 * /31 (RFC 3021) usa ambas direcciones; /32 es un solo host.
 */
int ipc_hosts_range(const char *ip_s, const char *mask_s, char *out, size_t n)
{
    uint32_t ip, mask;
    int prefix;
    int rc = parse_ip_mask(ip_s, mask_s, &ip, &mask, &prefix);
    if (rc != RC_OK) return rc;

    uint32_t net = ip & mask;
    uint32_t bc  = net | ~mask;
    uint32_t first, last;
    if (prefix == 32)      { first = last = net; }
    else if (prefix == 31) { first = net; last = bc; }
    else                   { first = net + 1; last = bc - 1; }

    if (first == last) {
        char buf[16];
        ipc_ip_to_str(first, buf, sizeof buf);
        return put(out, n, "%s", buf);
    }

    // Cuantos octetos de la izquierda son iguales
    int common = 0;
    while (common < 3 &&
           ((first >> (24 - 8 * common)) & 0xFFu) ==
           ((last  >> (24 - 8 * common)) & 0xFFu))
        common++;

    char head[16] = "", a[16] = "", b[16] = "";
    size_t hl = 0, al = 0, bl = 0;
    for (int i = 0; i < 4; i++) {
        unsigned fo = (first >> (24 - 8 * i)) & 0xFFu;
        unsigned lo = (last  >> (24 - 8 * i)) & 0xFFu;
        if (i < common) {
            hl += (size_t)snprintf(head + hl, sizeof head - hl, "%u.", fo);
        } else {
            const char *sep = (i > common) ? "." : "";
            al += (size_t)snprintf(a + al, sizeof a - al, "%s%u", sep, fo);
            bl += (size_t)snprintf(b + bl, sizeof b - bl, "%s%u", sep, lo);
        }
    }
    return put(out, n, "%s{%s-%s}", head, a, b);
}

/* 4. /N -> X.X.X.X  y  X.X.X.X -> /N */
int ipc_netmask(const char *mask_s, char *out, size_t n)
{
    uint32_t mask;
    int prefix = ipc_parse_mask(mask_s, &mask);
    if (prefix < 0) return RC_ERR_MASK;
    if (mask_s[0] == '/') {
        char buf[16];
        ipc_ip_to_str(mask, buf, sizeof buf);
        return put(out, n, "%s", buf);
    }
    return put(out, n, "/%d", prefix);
}

/* 5. Hosts utilizables = 2^(32-N) - 2  (/31 -> 2, /32 -> 1) */
int ipc_hosts_count(const char *mask_s, char *out, size_t n)
{
    int prefix = ipc_parse_mask(mask_s, NULL);
    if (prefix < 0) return RC_ERR_MASK;
    unsigned long long count;
    if (prefix == 32)      count = 1ULL;
    else if (prefix == 31) count = 2ULL;
    else                   count = (1ULL << (32 - prefix)) - 2ULL;
    return put(out, n, "%llu", count);
}

/* 6. Clase según el primer octeto */
int ipc_ip_class(const char *ip_s, char *out, size_t n)
{
    uint32_t ip;
    if (!ipc_parse_ip(ip_s, &ip)) return RC_ERR_IP;
    unsigned first = (ip >> 24) & 0xFFu;   //Solo revisa el primer byte
    char cls;
    if (first <= 127)      cls = 'A';
    else if (first <= 191) cls = 'B';
    else if (first <= 223) cls = 'C';
    else if (first <= 239) cls = 'D';
    else                   cls = 'E';
    return put(out, n, "Clase %c", cls);
}
