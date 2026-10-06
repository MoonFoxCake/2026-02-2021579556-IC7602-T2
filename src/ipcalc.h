/*
 * ipcalc.h - Interfaz de la calculadora de subredes (Persona 3)
 *
 * Reglas del contrato:
 *   - Reciben los argumentos como cadenas (tal cual los escribió el usuario).
 *   - ipcalc VALIDA la IP y la máscara (acepta "/29" y "255.255.255.248").
 *   - Escriben SOLO el valor en 'out' (sin "\n", sin etiquetas), con el
 *     formato exacto del enunciado. Ejemplos:
 *         ipc_broadcast   -> "10.8.2.7"
 *         ipc_hosts_range -> "10.8.2.{1-6}"
 *         ipc_netmask     -> "255.255.255.224"  (si entró /27)
 *                            "/24"              (si entró 255.255.255.0)
 *         ipc_hosts_count -> "6"
 *         ipc_ip_class    -> "Clase A"
 *   - NO usan printf, NO tienen main().
 *   - Devuelven RC_OK o RC_ERR_IP / RC_ERR_MASK / RC_ERR_INTERNAL (rc.h).
 *   - Deben ser seguras con hilos (sin variables globales mutables).
 */
#ifndef IPCALC_H
#define IPCALC_H

#include <stddef.h>
#include <stdint.h>
#include "rc.h"

/* Utilidades (las reutiliza routes.c, para no duplicar validadores). */
int  ipc_parse_ip  (const char *s, uint32_t *out);        /* 1 ok, 0 error   */
int  ipc_parse_mask(const char *s, uint32_t *mask_out);   /* prefijo, o < 0  */
void ipc_ip_to_str (uint32_t ip, char *buf, size_t n);

/* Primitivas 1 a 6 */
int ipc_broadcast  (const char *ip, const char *mask, char *out, size_t n);
int ipc_network    (const char *ip, const char *mask, char *out, size_t n);
int ipc_hosts_range(const char *ip, const char *mask, char *out, size_t n);
int ipc_netmask    (const char *mask,                 char *out, size_t n);
int ipc_hosts_count(const char *mask,                 char *out, size_t n);
int ipc_ip_class   (const char *ip,                   char *out, size_t n);

#endif /* IPCALC_H */
