/*
 * test_parser.c - Pruebas unitarias del parser (usa tests/stubs.c).
 * Cada caso: línea de entrada, texto que DEBE aparecer en la salida, y si se
 * espera PARSER_QUIT. Para "salida vacía" se usa expect = "".
 */
#include <stdio.h>
#include <string.h>
#include "../src/parser.h"

typedef struct { const char *in; const char *expect; int quit; } tc_t;

static const tc_t CASES[] = {
 /* --- Válidos: verifican despacho y orden de argumentos --- */
 {"GET BROADCAST IP 10.8.2.5 MASK /29\r\n",          "broadcast(10.8.2.5,/29)", 0},
 {"GET NETWORK NUMBER IP 10.8.2.5 MASK /29",         "network(10.8.2.5,/29)", 0},
 {"GET HOSTS RANGE IP 172.16.0.56 MASK 255.255.255.128", "range(172.16.0.56,255.255.255.128)", 0},
 {"GET NETMASK MASK /27",                            "netmask(/27)", 0},
 {"GET HOSTS COUNT MASK /29",                        "count(/29)", 0},
 {"GET IP CLASS 10.8.2.5",                           "class(10.8.2.5)", 0},
 {"GET RANDOM SUBNETS NETWORK NUMBER 10.0.0.0 MASK /8 NUMBER 3 SIZE /24",
                                                     "random(10.0.0.0,/8,3,/24)", 0},
 {"SET ROUTE casa NETWORK NUMBER 10.0.0.0 MASK /8 PRIORITY 5", "OK", 0},
 {"DEL ROUTE casa",                                  "OK", 0},
 {"ROUTE IP 8.8.8.8",                                "lookup(8.8.8.8,debug=0)", 0},
 {"ROUTE IP 8.8.8.8 DEBUG true",                     "lookup(8.8.8.8,debug=1)", 0},
 {"ROUTE IP 8.8.8.8 DEBUG FALSE",                    "lookup(8.8.8.8,debug=0)", 0},

 /* --- Tolerancia de formato --- */
 {"get broadcast ip 10.8.2.5 mask /29",              "broadcast(10.8.2.5,/29)", 0},
 {"  GET   BROADCAST \t IP  10.8.2.5   MASK /29  ",  "broadcast(10.8.2.5,/29)", 0},
 {"",                                                "", 0},
 {"   \r\n",                                         "", 0},
 {"exit",                                            "Bye", 1},

 /* --- Errores de IP / máscara (vienen de ipcalc) --- */
 {"GET BROADCAST IP bad MASK /29",                   "ERROR: dirección IP inválida", 0},
 {"GET BROADCAST IP 10.8.2.5 MASK bad",              "ERROR: máscara inválida", 0},
 {"GET NETMASK MASK bad",                            "ERROR: máscara inválida", 0},

 /* --- Errores de sintaxis --- */
 {"HOLA",                                            "ERROR: comando desconocido 'HOLA'", 0},
 {"GET",                                             "ERROR: comando incompleto", 0},
 {"GET FOO",                                         "ERROR: comando incompleto", 0},
 {"GET BROADCAST",                                   "ERROR: sintaxis inválida", 0},
 {"GET BROADCAST IP 10.8.2.5",                       "GET BROADCAST IP {ip} MASK", 0},
 {"GET BROADCAST IP 10.8.2.5 MSK /29",               "ERROR: sintaxis inválida", 0},
 {"GET BROADCAST IP 10.8.2.5 MASK /29 extra",        "ERROR: sintaxis inválida", 0},
 {"GET NETWORK IP 10.8.2.5 MASK /29",                "ERROR: sintaxis inválida", 0},
 {"GET HOSTS FOO",                                   "GET HOSTS COUNT", 0},
 {"DEL ROUTE",                                       "DEL ROUTE {nombre}", 0},
 {"ROUTE IP",                                        "ROUTE IP {ip}", 0},
 {"ROUTE IP 8.8.8.8 DEBUG",                          "ERROR: sintaxis inválida", 0},

 /* --- Validación de números / banderas --- */
 {"ROUTE IP 8.8.8.8 DEBUG quizas",                   "DEBUG debe ser true o false", 0},
 {"SET ROUTE r NETWORK NUMBER 10.0.0.0 MASK /8 PRIORITY abc", "PRIORITY debe ser un entero", 0},
 {"SET ROUTE r NETWORK NUMBER 10.0.0.0 MASK /8 PRIORITY -1",  "PRIORITY debe ser un entero", 0},
 {"SET ROUTE r NETWORK NUMBER 10.0.0.0 MASK /8 PRIORITY 99999999999999999999", "PRIORITY debe ser un entero", 0},
 {"SET ROUTE r@ NETWORK NUMBER 10.0.0.0 MASK /8 PRIORITY 1",  "nombre de ruta inválido", 0},
 {"GET RANDOM SUBNETS NETWORK NUMBER 10.0.0.0 MASK /8 NUMBER 0 SIZE /24",   "NUMBER debe ser un entero", 0},
 {"GET RANDOM SUBNETS NETWORK NUMBER 10.0.0.0 MASK /8 NUMBER 9999 SIZE /24","NUMBER debe ser un entero", 0},
 {"GET RANDOM SUBNETS NETWORK NUMBER 10.0.0.0 MASK /8 NUMBER x SIZE /24",   "NUMBER debe ser un entero", 0},

 /* --- Errores de rutas (vienen de routes) --- */
 {"SET ROUTE dup NETWORK NUMBER 10.0.0.0 MASK /8 PRIORITY 1", "ya existe una ruta", 0},
 {"DEL ROUTE nope",                                  "la ruta indicada no existe", 0},
 {"ROUTE IP 9.9.9.9",                                "ninguna ruta coincide", 0},

 /* --- Robustez --- */
 {"GET \x01\x02 BROADCAST",                          "caracteres no válidos", 0},
 {"\xff\xf4 GET IP CLASS 1.1.1.1",                   "caracteres no válidos", 0},
 {"A B C D E F G H I J K L M N O P Q",               "demasiadas palabras", 0},
};

int main(void)
{
    int fails = 0;
    size_t n = sizeof CASES / sizeof CASES[0];

    for (size_t i = 0; i < n; i++) {
        char out[PARSER_OUT_MAX];
        int r = parser_process_line(CASES[i].in, out, sizeof out);
        int ok = (r == (CASES[i].quit ? PARSER_QUIT : PARSER_OK));
        if (CASES[i].expect[0] == '\0') ok = ok && out[0] == '\0';
        else ok = ok && strstr(out, CASES[i].expect) != NULL;
        if (CASES[i].expect[0] != '\0') {          /* toda respuesta termina en CRLF */
            size_t l = strlen(out);
            ok = ok && l >= 2 && out[l-2] == '\r' && out[l-1] == '\n';
        }
        if (!ok) {
            fails++;
            printf("FAIL #%zu\n  entrada : %s\n  esperaba: %s\n  obtuvo  : %s\n",
                   i, CASES[i].in, CASES[i].expect, out);
        }
    }

    /* Línea gigante (desbordamiento) */
    char big[2000];
    memset(big, 'A', sizeof big - 1); big[sizeof big - 1] = '\0';
    char out[PARSER_OUT_MAX];
    parser_process_line(big, out, sizeof out);
    if (!strstr(out, "demasiado larga")) { fails++; printf("FAIL: línea larga\n"); }

    /* Buffer de salida diminuto: no debe desbordarse ni quedar sin '\0' */
    char tiny[8];
    memset(tiny, 'X', sizeof tiny);
    parser_process_line("GET BROADCAST IP bad MASK /29", tiny, sizeof tiny);
    if (memchr(tiny, '\0', sizeof tiny) == NULL) { fails++; printf("FAIL: buffer chico\n"); }

    printf("%zu casos + 2 de robustez, %d fallos\n", n, fails);
    return fails ? 1 : 0;
}
