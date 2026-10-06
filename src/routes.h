/*
 * routes.h - Subredes aleatorias y tabla de rutas (Persona 4)
 *
 * Reglas del contrato:
 *   - El parser ya validó: que 'name' sea un nombre razonable, que 'priority'
 *     sea un entero >= 0, que 'number' esté en [1, 256] y que 'debug' sea 0/1.
 *   - routes.c VALIDA las IPs y máscaras (puede usar ipc_parse_ip/ipc_parse_mask).
 *   - La tabla de rutas es global y compartida por todos los clientes:
 *     DEBE protegerse con un pthread_mutex_t.
 *   - Los textos multilínea pueden usar '\n' simple; el parser los convierte
 *     a "\r\n" para telnet.
 *   - Nunca escribir más de 'n' bytes en 'out'.
 */
#ifndef ROUTES_H
#define ROUTES_H

#include <stddef.h>
#include "rc.h"

/* Primitiva 7. 'out' = subredes separadas por espacio: "10.20.10.0/24 ..." */
int routes_random_subnets(const char *network, const char *mask,
                          long number, const char *size,
                          char *out, size_t n);

/* Primitiva 8. La red se normaliza con la mascara antes de guardarla.
 * RC_OK, RC_ERR_IP, RC_ERR_MASK o RC_ERR_EXISTS. */
int routes_set(const char *name, const char *network, const char *mask,
               long priority);

/* Primitiva 9. RC_OK o RC_ERR_NOT_FOUND. */
int routes_del(const char *name);

/* Primitiva 10. Usa longest-prefix match. En un empate de prefijo gana la
 * prioridad numericamente menor; si tambien empata, gana la ruta mas antigua.
 * 'out' = nombre de la ruta (o el detalle si debug != 0).
 * RC_ERR_NO_ROUTE si ninguna ruta coincide. */
int routes_lookup(const char *ip, int debug, char *out, size_t n);

#endif /* ROUTES_H */
