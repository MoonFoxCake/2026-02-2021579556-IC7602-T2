/*
 * routes.c - Subredes aleatorias y tabla de rutas IPv4 compartida.
 *
 * La tabla usa longest-prefix match. Si dos rutas tienen el mismo prefijo,
 * el numero de prioridad mas bajo gana (igual que una metrica de routing).
 * Si ambos valores empatan, se conserva la ruta que fue agregada primero.
 */
#include <pthread.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "ipcalc.h"
#include "routes.h"

#define ROUTES_MAX       256
#define ROUTE_NAME_MAX    32
#define RANDOM_MAX       256

typedef struct {
    char name[ROUTE_NAME_MAX + 1];
    uint32_t network;
    uint32_t mask;
    int prefix;
    long priority;
} route_t;

static route_t g_routes[ROUTES_MAX];
static size_t g_route_count;
static pthread_mutex_t g_routes_mu = PTHREAD_MUTEX_INITIALIZER;

/* Estado independiente por hilo: rand() comparte estado y no sirve aqui. */
static _Thread_local uint64_t g_rng_state;
static atomic_uint_fast64_t g_seed_sequence = 1;

static uint64_t mix64(uint64_t value)
{
    value += UINT64_C(0x9e3779b97f4a7c15);
    value = (value ^ (value >> 30)) * UINT64_C(0xbf58476d1ce4e5b9);
    value = (value ^ (value >> 27)) * UINT64_C(0x94d049bb133111eb);
    return value ^ (value >> 31);
}

static uint64_t random_u64(void)
{
    if (g_rng_state == 0) {
        struct timespec now;
        clock_gettime(CLOCK_REALTIME, &now);
        uint64_t sequence = atomic_fetch_add(&g_seed_sequence, 1);
        uint64_t seed = (uint64_t)now.tv_sec ^ ((uint64_t)now.tv_nsec << 32);
        seed ^= (uint64_t)(uintptr_t)&g_rng_state;
        g_rng_state = mix64(seed ^ sequence);
        if (g_rng_state == 0) g_rng_state = UINT64_C(0x2545f4914f6cdd1d);
    }

    /* xorshift64*: rapido y suficiente para seleccionar subredes al azar. */
    uint64_t x = g_rng_state;
    x ^= x >> 12;
    x ^= x << 25;
    x ^= x >> 27;
    g_rng_state = x;
    return x * UINT64_C(0x2545f4914f6cdd1d);
}

/* Sorteo uniforme en [0, bound), sin sesgo por usar modulo directamente. */
static uint64_t random_below(uint64_t bound)
{
    uint64_t threshold = (uint64_t)(0 - bound) % bound;
    uint64_t value;
    do value = random_u64(); while (value < threshold);
    return value % bound;
}

static int appendf(char *out, size_t n, size_t *used, const char *fmt, ...)
{
    if (out == NULL || used == NULL || n == 0 || *used >= n) return 0;

    va_list ap;
    va_start(ap, fmt);
    int written = vsnprintf(out + *used, n - *used, fmt, ap);
    va_end(ap);

    if (written < 0 || (size_t)written >= n - *used) {
        out[0] = '\0';
        return 0;
    }
    *used += (size_t)written;
    return 1;
}

static void ip_to_binary(uint32_t ip, char out[36])
{
    size_t pos = 0;
    for (int bit = 31; bit >= 0; bit--) {
        out[pos++] = (ip & (UINT32_C(1) << bit)) ? '1' : '0';
        if (bit == 24 || bit == 16 || bit == 8) out[pos++] = '.';
    }
    out[pos] = '\0';
}

int routes_random_subnets(const char *network, const char *mask,
                          long number, const char *size,
                          char *out, size_t n)
{
    if (out == NULL || n == 0) return RC_ERR_INTERNAL;
    out[0] = '\0';

    uint32_t ip, base_mask, subnet_mask;
    if (!ipc_parse_ip(network, &ip)) return RC_ERR_IP;

    int base_prefix = ipc_parse_mask(mask, &base_mask);
    if (base_prefix < 0) return RC_ERR_MASK;
    int subnet_prefix = ipc_parse_mask(size, &subnet_mask);
    if (subnet_prefix < 0) return RC_ERR_MASK;

    if (number < 1 || number > RANDOM_MAX || subnet_prefix < base_prefix)
        return RC_ERR_RANGE;

    int extra_bits = subnet_prefix - base_prefix;
    uint64_t total = UINT64_C(1) << extra_bits; /* extra_bits esta en [0,32] */
    if ((uint64_t)number > total) return RC_ERR_RANGE;

    uint32_t base_network = ip & base_mask;
    uint64_t chosen[RANDOM_MAX];
    size_t chosen_count = 0;

    while (chosen_count < (size_t)number) {
        uint64_t candidate = random_below(total);
        int duplicate = 0;
        for (size_t i = 0; i < chosen_count; i++) {
            if (chosen[i] == candidate) {
                duplicate = 1;
                break;
            }
        }
        if (!duplicate) chosen[chosen_count++] = candidate;
    }

    size_t used = 0;
    for (size_t i = 0; i < chosen_count; i++) {
        unsigned host_bits = 32u - (unsigned)subnet_prefix;
        uint32_t subnet = base_network;
        if (host_bits < 32u)
            subnet |= (uint32_t)(chosen[i] << host_bits);
        subnet &= subnet_mask;

        char address[16];
        ipc_ip_to_str(subnet, address, sizeof address);
        if (!appendf(out, n, &used, "%s%s/%d",
                     i == 0 ? "" : " ", address, subnet_prefix))
            return RC_ERR_INTERNAL;
    }
    return RC_OK;
}

int routes_set(const char *name, const char *network, const char *mask,
               long priority)
{
    uint32_t ip, parsed_mask;
    if (!ipc_parse_ip(network, &ip)) return RC_ERR_IP;
    int prefix = ipc_parse_mask(mask, &parsed_mask);
    if (prefix < 0) return RC_ERR_MASK;
    if (name == NULL || name[0] == '\0') return RC_ERR_INTERNAL;

    pthread_mutex_lock(&g_routes_mu);
    for (size_t i = 0; i < g_route_count; i++) {
        if (strcmp(g_routes[i].name, name) == 0) {
            pthread_mutex_unlock(&g_routes_mu);
            return RC_ERR_EXISTS;
        }
    }
    if (g_route_count == ROUTES_MAX) {
        pthread_mutex_unlock(&g_routes_mu);
        return RC_ERR_INTERNAL;
    }

    route_t *route = &g_routes[g_route_count++];
    snprintf(route->name, sizeof route->name, "%s", name);
    route->network = ip & parsed_mask; /* normaliza una IP no alineada */
    route->mask = parsed_mask;
    route->prefix = prefix;
    route->priority = priority;
    pthread_mutex_unlock(&g_routes_mu);
    return RC_OK;
}

int routes_del(const char *name)
{
    if (name == NULL) return RC_ERR_NOT_FOUND;

    pthread_mutex_lock(&g_routes_mu);
    for (size_t i = 0; i < g_route_count; i++) {
        if (strcmp(g_routes[i].name, name) != 0) continue;
        if (i + 1 < g_route_count) {
            memmove(&g_routes[i], &g_routes[i + 1],
                    (g_route_count - i - 1) * sizeof g_routes[0]);
        }
        g_route_count--;
        pthread_mutex_unlock(&g_routes_mu);
        return RC_OK;
    }
    pthread_mutex_unlock(&g_routes_mu);
    return RC_ERR_NOT_FOUND;
}

int routes_lookup(const char *ip_text, int debug, char *out, size_t n)
{
    if (out == NULL || n == 0) return RC_ERR_INTERNAL;
    out[0] = '\0';

    uint32_t ip;
    if (!ipc_parse_ip(ip_text, &ip)) return RC_ERR_IP;

    pthread_mutex_lock(&g_routes_mu);
    size_t best = ROUTES_MAX;
    for (size_t i = 0; i < g_route_count; i++) {
        if ((ip & g_routes[i].mask) != g_routes[i].network) continue;
        if (best == ROUTES_MAX ||
            g_routes[i].prefix > g_routes[best].prefix ||
            (g_routes[i].prefix == g_routes[best].prefix &&
             g_routes[i].priority < g_routes[best].priority)) {
            best = i;
        }
    }

    if (best == ROUTES_MAX) {
        pthread_mutex_unlock(&g_routes_mu);
        return RC_ERR_NO_ROUTE;
    }

    size_t used = 0;
    int ok = 1;
    if (!debug) {
        ok = appendf(out, n, &used, "%s", g_routes[best].name);
    } else {
        char binary[36];
        ip_to_binary(ip, binary);
        ok = appendf(out, n, &used, "Evaluando %s (%s)\n", ip_text, binary);

        size_t omitted = 0;
        for (size_t i = 0; ok && i < g_route_count; i++) {
            char network[16], mask[16], result[16];
            char line[256];
            uint32_t masked = ip & g_routes[i].mask;
            ipc_ip_to_str(g_routes[i].network, network, sizeof network);
            ipc_ip_to_str(g_routes[i].mask, mask, sizeof mask);
            ipc_ip_to_str(masked, result, sizeof result);
            int line_len = snprintf(
                line, sizeof line,
                "  ruta %s %s/%d: %s & %s = %s -> %s "
                "(prefijo %d, prioridad %ld)\n",
                g_routes[i].name, network, g_routes[i].prefix,
                ip_text, mask, result,
                masked == g_routes[i].network ? "COINCIDE" : "NO coincide",
                g_routes[i].prefix, g_routes[i].priority);
            if (line_len < 0 || (size_t)line_len >= sizeof line) {
                ok = 0;
                break;
            }

            /* Reserva espacio para el resumen y la ruta elegida. */
            if (used + (size_t)line_len + 256 >= n) {
                omitted = g_route_count - i;
                break;
            }
            ok = appendf(out, n, &used, "%s", line);
        }
        if (ok && omitted > 0) {
            ok = appendf(out, n, &used,
                         "  ... %zu ruta(s) omitida(s) por limite de salida\n",
                         omitted);
        }
        if (ok) {
            ok = appendf(out, n, &used,
                         "Seleccionada: %s (prefijo %d, prioridad %ld; "
                         "longest prefix match y menor prioridad en empate)\n%s",
                         g_routes[best].name, g_routes[best].prefix,
                         g_routes[best].priority, g_routes[best].name);
        }
    }

    pthread_mutex_unlock(&g_routes_mu);
    return ok ? RC_OK : RC_ERR_INTERNAL;
}
