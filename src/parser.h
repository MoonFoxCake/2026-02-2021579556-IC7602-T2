/*
 * parser.h - Interfaz del parser de comandos (Persona 2)
 *
 * Uso desde server.c (Persona 1), por cada línea recibida del cliente:
 *
 *     char out[PARSER_OUT_MAX];
 *     int  r = parser_process_line(linea, out, sizeof out);
 *     if (out[0] != '\0') send(fd, out, strlen(out), 0);
 *     if (r == PARSER_QUIT) close(fd);
 *
 * Garantías:
 *   - 'line' puede traer "\r\n" al final; el parser lo elimina.
 *   - 'out' siempre queda terminado en '\0' y NUNCA se desborda.
 *   - 'out' ya termina en "\r\n" (el servidor no agrega nada).
 *   - Línea vacía -> out queda vacío (el servidor no envía nada).
 *   - Los errores empiezan con "ERROR:".
 *   - Es segura con hilos (sin estado global).
 */
#ifndef PARSER_H
#define PARSER_H

#include <stddef.h>

#define PARSER_OK        0
#define PARSER_QUIT      1      /* el cliente escribió EXIT o QUIT */

#define MAX_LINE         512    /* longitud máxima aceptada de una línea   */
#define PARSER_OUT_MAX   8192   /* tamaño de buffer recomendado para 'out' */

int parser_process_line(const char *line, char *out, size_t outlen);

#endif /* PARSER_H */
