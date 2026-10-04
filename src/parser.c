/*
 * parser.c - Tokenización, despacho y validación de las 10 primitivas.
 *
 * Diseño (guiado por tabla):
 *   Cada comando se describe con un "patrón" de palabras, donde "$" marca un
 *   argumento. Por ejemplo:
 *
 *       { "GET","BROADCAST","IP","$","MASK","$" }
 *
 *   El parser compara los tokens contra cada patrón; si coincide, guarda los
 *   "$" en args[] y llama al handler del comando. Agregar un comando nuevo
 *   es agregar una fila a la tabla.
 *
 * Reparto de responsabilidades:
 *   - Parser:  estructura del comando, palabras clave, números (NUMBER,
 *              PRIORITY), DEBUG, nombre de ruta, formato de salida.
 *   - ipcalc / routes: validar IP y máscara; devuelven un código (rc.h).
 */
#define _GNU_SOURCE
#include <ctype.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "parser.h"
#include "ipcalc.h"
#include "routes.h"
#include "rc.h"

#define MAX_TOKENS    16
#define MAX_PATTERN   16
#define MAX_ARGS       8
#define RES_MAX     8192
#define MAX_NAME      32
#define MAX_PRIORITY  1000000L
#define MAX_SUBNETS   256L

typedef int (*handler_fn)(char *const args[], char *res, size_t n);

typedef struct {
    const char *words[MAX_PATTERN]; /* palabras clave y "$", terminado en NULL */
    const char *usage;              /* texto que se muestra en los errores     */
    handler_fn  fn;
} command_t;

/* ------------------------------------------------------------------ */
/* Utilidades                                                          */
/* ------------------------------------------------------------------ */

/* snprintf acumulativo: agrega al final de buf sin desbordar. */
static void appendf(char *buf, size_t n, const char *fmt, ...)
{
    size_t used = strlen(buf);
    if (used >= n - 1) return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf + used, n - used, fmt, ap);
    va_end(ap);
}

/* Convierte la cadena a entero en [min,max]. 1 = ok, 0 = inválido. */
static int parse_long(const char *s, long min, long max, long *out)
{
    if (!s || !*s) return 0;
    char *end = NULL;
    errno = 0;
    long v = strtol(s, &end, 10);
    if (errno != 0 || *end != '\0' || v < min || v > max) return 0;
    *out = v;
    return 1;
}

/* Nombre de ruta: 1..MAX_NAME caracteres de [A-Za-z0-9_.-] */
static int valid_name(const char *s)
{
    size_t len = strlen(s);
    if (len == 0 || len > MAX_NAME) return 0;
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)s[i];
        if (!isalnum(c) && c != '_' && c != '-' && c != '.') return 0;
    }
    return 1;
}

/* Traduce un código de retorno de ipcalc/routes a un mensaje de usuario. */
static void rc_to_message(int rc, char *res, size_t n)
{
    res[0] = '\0';
    switch (rc) {
    case RC_ERR_IP:
        appendf(res, n, "ERROR: dirección IP inválida "
                        "(formato X.X.X.X, cada octeto entre 0 y 255)");
        break;
    case RC_ERR_MASK:
        appendf(res, n, "ERROR: máscara inválida (use /N con N entre 0 y 32, "
                        "o X.X.X.X con bits contiguos, ej. 255.255.255.0)");
        break;
    case RC_ERR_NOT_FOUND:
        appendf(res, n, "ERROR: la ruta indicada no existe");
        break;
    case RC_ERR_EXISTS:
        appendf(res, n, "ERROR: ya existe una ruta con ese nombre "
                        "(use DEL ROUTE primero)");
        break;
    case RC_ERR_RANGE:
        appendf(res, n, "ERROR: parámetros fuera de rango (SIZE debe ser igual "
                        "o más específica que MASK, y NUMBER no puede superar "
                        "las subredes disponibles)");
        break;
    case RC_ERR_NO_ROUTE:
        appendf(res, n, "ERROR: ninguna ruta coincide con esa dirección IP");
        break;
    default:
        appendf(res, n, "ERROR: error interno del servidor");
        break;
    }
}

/* ------------------------------------------------------------------ */
/* Handlers: uno por primitiva. Escriben el resultado en res.          */
/* args[] contiene los "$" del patrón, en orden.                       */
/* ------------------------------------------------------------------ */

/* 1 */ static int h_broadcast(char *const a[], char *res, size_t n)
{ return ipc_broadcast(a[0], a[1], res, n); }

/* 2 */ static int h_network(char *const a[], char *res, size_t n)
{ return ipc_network(a[0], a[1], res, n); }

/* 3 */ static int h_hosts_range(char *const a[], char *res, size_t n)
{ return ipc_hosts_range(a[0], a[1], res, n); }

/* 4 */ static int h_netmask(char *const a[], char *res, size_t n)
{ return ipc_netmask(a[0], res, n); }

/* 5 */ static int h_hosts_count(char *const a[], char *res, size_t n)
{ return ipc_hosts_count(a[0], res, n); }

/* 6 */ static int h_ip_class(char *const a[], char *res, size_t n)
{ return ipc_ip_class(a[0], res, n); }

/* 7: args = red, máscara, number, size */
static int h_random_subnets(char *const a[], char *res, size_t n)
{
    long number;
    if (!parse_long(a[2], 1, MAX_SUBNETS, &number)) {
        res[0] = '\0';
        appendf(res, n, "ERROR: NUMBER debe ser un entero entre 1 y %ld",
                MAX_SUBNETS);
        return RC_ERR_ARG;
    }
    return routes_random_subnets(a[0], a[1], number, a[3], res, n);
}

/* 8: args = nombre, red, máscara, prioridad */
static int h_set_route(char *const a[], char *res, size_t n)
{
    long prio;
    res[0] = '\0';
    if (!valid_name(a[0])) {
        appendf(res, n, "ERROR: nombre de ruta inválido (1 a %d caracteres: "
                        "letras, números, '_', '-' o '.')", MAX_NAME);
        return RC_ERR_ARG;
    }
    if (!parse_long(a[3], 0, MAX_PRIORITY, &prio)) {
        appendf(res, n, "ERROR: PRIORITY debe ser un entero entre 0 y %ld",
                MAX_PRIORITY);
        return RC_ERR_ARG;
    }
    int rc = routes_set(a[0], a[1], a[2], prio);
    if (rc == RC_OK) appendf(res, n, "OK");
    return rc;
}

/* 9: args = nombre */
static int h_del_route(char *const a[], char *res, size_t n)
{
    res[0] = '\0';
    if (!valid_name(a[0])) {
        appendf(res, n, "ERROR: nombre de ruta inválido");
        return RC_ERR_ARG;
    }
    int rc = routes_del(a[0]);
    if (rc == RC_OK) appendf(res, n, "OK");
    return rc;
}

/* 10: args = ip, [debug]. a[1] es NULL si no se especificó DEBUG. */
static int h_route_ip(char *const a[], char *res, size_t n)
{
    int debug = 0;                          /* por defecto: false */
    res[0] = '\0';
    if (a[1] != NULL) {
        if (strcasecmp(a[1], "true") == 0)       debug = 1;
        else if (strcasecmp(a[1], "false") == 0) debug = 0;
        else {
            appendf(res, n, "ERROR: DEBUG debe ser true o false");
            return RC_ERR_ARG;
        }
    }
    return routes_lookup(a[0], debug, res, n);
}

/* ------------------------------------------------------------------ */
/* Tabla de comandos                                                   */
/* ------------------------------------------------------------------ */

#define U_ROUTE_IP "ROUTE IP {ip} [DEBUG {true|false}]"

static const command_t COMMANDS[] = {
 { {"GET","BROADCAST","IP","$","MASK","$",NULL},
   "GET BROADCAST IP {ip} MASK {mascara}", h_broadcast },
 { {"GET","NETWORK","NUMBER","IP","$","MASK","$",NULL},
   "GET NETWORK NUMBER IP {ip} MASK {mascara}", h_network },
 { {"GET","HOSTS","RANGE","IP","$","MASK","$",NULL},
   "GET HOSTS RANGE IP {ip} MASK {mascara}", h_hosts_range },
 { {"GET","NETMASK","MASK","$",NULL},
   "GET NETMASK MASK {mascara}", h_netmask },
 { {"GET","HOSTS","COUNT","MASK","$",NULL},
   "GET HOSTS COUNT MASK {mascara}", h_hosts_count },
 { {"GET","IP","CLASS","$",NULL},
   "GET IP CLASS {ip}", h_ip_class },
 { {"GET","RANDOM","SUBNETS","NETWORK","NUMBER","$","MASK","$",
    "NUMBER","$","SIZE","$",NULL},
   "GET RANDOM SUBNETS NETWORK NUMBER {red} MASK {mascara} "
   "NUMBER {cantidad} SIZE {mascara}", h_random_subnets },
 { {"SET","ROUTE","$","NETWORK","NUMBER","$","MASK","$","PRIORITY","$",NULL},
   "SET ROUTE {nombre} NETWORK NUMBER {red} MASK {mascara} "
   "PRIORITY {numero}", h_set_route },
 { {"DEL","ROUTE","$",NULL},
   "DEL ROUTE {nombre}", h_del_route },
 { {"ROUTE","IP","$",NULL},
   U_ROUTE_IP, h_route_ip },
 { {"ROUTE","IP","$","DEBUG","$",NULL},
   U_ROUTE_IP, h_route_ip },
};
#define N_COMMANDS (sizeof COMMANDS / sizeof COMMANDS[0])

static int pattern_len(const command_t *c)
{
    int k = 0;
    while (c->words[k]) k++;
    return k;
}

/* ¿Los tokens calzan exactamente con el patrón? Llena args[]. */
static int match(const command_t *c, char *tok[], int ntok, char *args[])
{
    if (pattern_len(c) != ntok) return 0;
    int na = 0;
    for (int i = 0; i < ntok; i++) {
        if (strcmp(c->words[i], "$") == 0) {
            if (na < MAX_ARGS) args[na++] = tok[i];
        } else if (strcasecmp(c->words[i], tok[i]) != 0) {
            return 0;
        }
    }
    return 1;
}

/* Cuántas palabras clave iniciales coinciden (se detiene en "$"). */
static int prefix_score(const command_t *c, char *tok[], int ntok)
{
    int s = 0;
    for (int i = 0; i < ntok && c->words[i]; i++) {
        if (strcmp(c->words[i], "$") == 0) break;
        if (strcasecmp(c->words[i], tok[i]) != 0) break;
        s++;
    }
    return s;
}

/* Arma el mensaje de error cuando ningún patrón calzó. */
static void build_syntax_error(char *tok[], int ntok, char *res, size_t n)
{
    int best = 0;
    for (size_t i = 0; i < N_COMMANDS; i++) {
        int s = prefix_score(&COMMANDS[i], tok, ntok);
        if (s > best) best = s;
    }

    res[0] = '\0';
    if (best >= 2) {
        /* Sabemos de qué comando se trata; mostramos su uso. */
        appendf(res, n, "ERROR: sintaxis inválida (faltan o sobran "
                        "argumentos, o una palabra clave está mal). Uso:");
        const char *last = NULL;
        for (size_t i = 0; i < N_COMMANDS; i++) {
            if (prefix_score(&COMMANDS[i], tok, ntok) == best &&
                (last == NULL || strcmp(last, COMMANDS[i].usage) != 0)) {
                appendf(res, n, "\n  %s", COMMANDS[i].usage);
                last = COMMANDS[i].usage;
            }
        }
        return;
    }

    /* ¿La primera palabra es un verbo conocido (GET/SET/DEL/ROUTE)? */
    int known = 0;
    for (size_t i = 0; i < N_COMMANDS; i++)
        if (strcasecmp(COMMANDS[i].words[0], tok[0]) == 0) known = 1;

    if (known) {
        appendf(res, n, "ERROR: comando incompleto o desconocido después de "
                        "'%s'. Comandos válidos con ese verbo:", tok[0]);
        for (size_t i = 0; i < N_COMMANDS; i++) {
            if (strcasecmp(COMMANDS[i].words[0], tok[0]) != 0) continue;
            if (i > 0 && strcmp(COMMANDS[i-1].usage, COMMANDS[i].usage) == 0)
                continue;
            appendf(res, n, "\n  %s", COMMANDS[i].usage);
        }
    } else {
        appendf(res, n, "ERROR: comando desconocido '%s'. "
                        "Debe comenzar con GET, SET, DEL o ROUTE", tok[0]);
    }
}

/* ------------------------------------------------------------------ */
/* Salida: copia a 'out' sin desbordar y convierte "\n" en "\r\n".     */
/* ------------------------------------------------------------------ */
static void emit(char *out, size_t outlen, const char *text)
{
    size_t o = 0;
    for (const char *p = text; *p && o + 3 < outlen; p++) {
        if (*p == '\n' && (p == text || p[-1] != '\r')) out[o++] = '\r';
        out[o++] = *p;
    }
    if (o + 3 <= outlen) { out[o++] = '\r'; out[o++] = '\n'; }
    out[o] = '\0';
}

/* ------------------------------------------------------------------ */
/* Punto de entrada                                                    */
/* ------------------------------------------------------------------ */
int parser_process_line(const char *line, char *out, size_t outlen)
{
    if (out == NULL || outlen == 0) return PARSER_OK;
    out[0] = '\0';
    if (line == NULL) return PARSER_OK;

    char res[RES_MAX];

    /* 1. Quitar \r\n del final y validar longitud */
    size_t len = strlen(line);
    while (len > 0 && (line[len-1] == '\n' || line[len-1] == '\r')) len--;
    if (len >= MAX_LINE) {
        snprintf(res, sizeof res,
                 "ERROR: línea demasiado larga (máximo %d caracteres)",
                 MAX_LINE - 1);
        emit(out, outlen, res);
        return PARSER_OK;
    }

    char buf[MAX_LINE];
    memcpy(buf, line, len);
    buf[len] = '\0';

    /* 2. Rechazar bytes no imprimibles (p. ej. secuencias IAC de telnet) */
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)buf[i];
        if (c >= 0x80 || (iscntrl(c) && c != '\t')) {
            emit(out, outlen, "ERROR: la línea contiene caracteres no válidos");
            return PARSER_OK;
        }
    }

    /* 3. Tokenizar (strtok_r: segura con hilos) */
    char *tok[MAX_TOKENS];
    int   ntok = 0;
    char *save = NULL;
    for (char *t = strtok_r(buf, " \t", &save); t;
         t = strtok_r(NULL, " \t", &save)) {
        if (ntok == MAX_TOKENS) {
            emit(out, outlen, "ERROR: demasiadas palabras en el comando");
            return PARSER_OK;
        }
        tok[ntok++] = t;
    }
    if (ntok == 0) return PARSER_OK;            /* línea vacía: no responder */

    /* Extra (no está en el enunciado): salir con EXIT / QUIT */
    if (ntok == 1 && (strcasecmp(tok[0], "EXIT") == 0 ||
                      strcasecmp(tok[0], "QUIT") == 0)) {
        emit(out, outlen, "Bye");
        return PARSER_QUIT;
    }

    /* 4. Buscar el patrón que calza y ejecutar el handler */
    for (size_t i = 0; i < N_COMMANDS; i++) {
        char *args[MAX_ARGS] = { NULL };
        if (!match(&COMMANDS[i], tok, ntok, args)) continue;

        res[0] = '\0';
        int rc = COMMANDS[i].fn(args, res, sizeof res);
        if (rc == RC_OK || rc == RC_ERR_ARG)
            emit(out, outlen, res);     /* RC_ERR_ARG: res ya trae el mensaje */
        else {
            rc_to_message(rc, res, sizeof res);
            emit(out, outlen, res);
        }
        return PARSER_OK;
    }

    /* 5. Nada calzó: explicar por qué */
    build_syntax_error(tok, ntok, res, sizeof res);
    emit(out, outlen, res);
    return PARSER_OK;
}
