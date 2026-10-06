/* Pruebas unitarias de routes.c, con solo las utilidades de ipcalc simuladas. */
#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/ipcalc.h"
#include "../src/routes.h"

int ipc_parse_ip(const char *s, uint32_t *out)
{
    unsigned a, b, c, d;
    char extra;
    if (!s || sscanf(s, "%u.%u.%u.%u%c", &a, &b, &c, &d, &extra) != 4 ||
        a > 255 || b > 255 || c > 255 || d > 255)
        return 0;
    *out = ((uint32_t)a << 24) | ((uint32_t)b << 16) |
           ((uint32_t)c << 8) | (uint32_t)d;
    return 1;
}

int ipc_parse_mask(const char *s, uint32_t *out)
{
    if (!s || !*s) return -1;
    if (s[0] == '/') {
        char *end = NULL;
        long prefix = strtol(s + 1, &end, 10);
        if (s[1] == '\0' || *end != '\0' || prefix < 0 || prefix > 32)
            return -1;
        *out = prefix == 0 ? 0 : UINT32_MAX << (32 - prefix);
        return (int)prefix;
    }

    uint32_t mask;
    if (!ipc_parse_ip(s, &mask)) return -1;
    int prefix = 0, saw_zero = 0;
    for (int bit = 31; bit >= 0; bit--) {
        if (mask & (UINT32_C(1) << bit)) {
            if (saw_zero) return -1;
            prefix++;
        } else {
            saw_zero = 1;
        }
    }
    *out = mask;
    return prefix;
}

void ipc_ip_to_str(uint32_t ip, char *out, size_t n)
{
    snprintf(out, n, "%u.%u.%u.%u", (ip >> 24) & 255u,
             (ip >> 16) & 255u, (ip >> 8) & 255u, ip & 255u);
}

static int failures;

#define CHECK(condition, message) do { \
    if (!(condition)) { \
        failures++; \
        printf("FAIL: %s (linea %d)\n", message, __LINE__); \
    } \
} while (0)

static void test_random_subnets(void)
{
    char out[8192];
    CHECK(routes_random_subnets("bad", "/8", 3, "/24", out, sizeof out) == RC_ERR_IP,
          "rechaza IP invalida");
    CHECK(routes_random_subnets("10.0.0.0", "bad", 3, "/24", out, sizeof out) == RC_ERR_MASK,
          "rechaza mascara base invalida");
    CHECK(routes_random_subnets("10.0.0.0", "/8", 3, "bad", out, sizeof out) == RC_ERR_MASK,
          "rechaza SIZE invalido");
    CHECK(routes_random_subnets("10.0.0.0", "/24", 1, "/8", out, sizeof out) == RC_ERR_RANGE,
          "rechaza SIZE menos especifico");
    CHECK(routes_random_subnets("10.0.0.0", "/30", 5, "/32", out, sizeof out) == RC_ERR_RANGE,
          "rechaza mas subredes que las disponibles");

    CHECK(routes_random_subnets("10.0.0.99", "/30", 4, "/32", out, sizeof out) == RC_OK,
          "genera todas las subredes /32");
    char copy[8192];
    snprintf(copy, sizeof copy, "%s", out);
    char *items[4] = {0};
    size_t count = 0;
    char *save = NULL;
    for (char *token = strtok_r(copy, " ", &save); token;
         token = strtok_r(NULL, " ", &save)) {
        CHECK(count < 4, "no genera mas resultados de los pedidos");
        if (count < 4) items[count++] = token;
    }
    CHECK(count == 4, "genera cuatro resultados");
    for (size_t i = 0; i < count; i++) {
        CHECK(strstr(items[i], "10.0.0.") == items[i], "subred dentro de la red base");
        CHECK(strstr(items[i], "/32") != NULL, "incluye prefijo SIZE");
        for (size_t j = i + 1; j < count; j++)
            CHECK(strcmp(items[i], items[j]) != 0, "subredes sin duplicados");
    }

    char tiny[4];
    CHECK(routes_random_subnets("10.0.0.0", "/8", 1, "/24", tiny, sizeof tiny) == RC_ERR_INTERNAL,
          "detecta buffer insuficiente");
    CHECK(tiny[0] == '\0', "buffer insuficiente queda terminado");
}

static void test_routes(void)
{
    char out[8192];
    CHECK(routes_set("bad-ip", "999.1.1.1", "/8", 1) == RC_ERR_IP,
          "SET valida IP");
    CHECK(routes_set("bad-mask", "10.0.0.0", "255.0.255.0", 1) == RC_ERR_MASK,
          "SET valida mascara contigua");

    CHECK(routes_set("default", "0.0.0.0", "/0", 100) == RC_OK,
          "agrega ruta por defecto");
    CHECK(routes_set("corp", "10.0.0.0", "/8", 50) == RC_OK,
          "agrega ruta /8");
    CHECK(routes_set("office-old", "10.20.0.0", "/16", 20) == RC_OK,
          "agrega primera ruta /16");
    CHECK(routes_set("office-best", "10.20.99.7", "/16", 5) == RC_OK,
          "normaliza y agrega segunda ruta /16");
    CHECK(routes_set("office-best", "10.20.0.0", "/16", 1) == RC_ERR_EXISTS,
          "rechaza nombre duplicado");

    CHECK(routes_lookup("10.20.30.40", 0, out, sizeof out) == RC_OK,
          "encuentra ruta");
    CHECK(strcmp(out, "office-best") == 0,
          "longest prefix y menor prioridad resuelven empate");

    CHECK(routes_lookup("10.30.1.2", 0, out, sizeof out) == RC_OK &&
          strcmp(out, "corp") == 0, "prefiere /8 sobre default");
    CHECK(routes_lookup("8.8.8.8", 0, out, sizeof out) == RC_OK &&
          strcmp(out, "default") == 0, "usa ruta por defecto");

    CHECK(routes_lookup("10.20.30.40", 1, out, sizeof out) == RC_OK,
          "DEBUG responde correctamente");
    CHECK(strstr(out, "Evaluando 10.20.30.40") != NULL,
          "DEBUG muestra IP y binario");
    CHECK(strstr(out, "ruta office-best") != NULL &&
          strstr(out, "COINCIDE") != NULL, "DEBUG muestra rutas evaluadas");
    CHECK(strstr(out, "Seleccionada: office-best") != NULL,
          "DEBUG explica seleccion");
    CHECK(strstr(out, "longest prefix match") != NULL,
          "DEBUG identifica el algoritmo");

    char tiny[8];
    CHECK(routes_lookup("10.20.30.40", 1, tiny, sizeof tiny) == RC_ERR_INTERNAL,
          "DEBUG detecta buffer insuficiente");
    CHECK(tiny[0] == '\0', "DEBUG no deja salida parcial al desbordar");

    CHECK(routes_del("office-best") == RC_OK, "elimina ruta");
    CHECK(routes_lookup("10.20.30.40", 0, out, sizeof out) == RC_OK &&
          strcmp(out, "office-old") == 0, "fallback tras DEL");
    CHECK(routes_del("office-best") == RC_ERR_NOT_FOUND,
          "DEL reporta ruta inexistente");

    CHECK(routes_del("office-old") == RC_OK, "limpia office-old");
    CHECK(routes_del("corp") == RC_OK, "limpia corp");
    CHECK(routes_del("default") == RC_OK, "limpia default");
    CHECK(routes_lookup("8.8.8.8", 0, out, sizeof out) == RC_ERR_NO_ROUTE,
          "reporta ausencia de ruta");
    CHECK(routes_lookup("bad", 0, out, sizeof out) == RC_ERR_IP,
          "ROUTE valida IP");
}

int main(void)
{
    test_random_subnets();
    test_routes();
    printf("routes: %d fallos\n", failures);
    return failures ? 1 : 0;
}
