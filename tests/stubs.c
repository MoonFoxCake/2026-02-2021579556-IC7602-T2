/*
 * stubs.c - Implementaciones FALSAS de ipcalc y routes, solo para probar el
 * parser de forma aislada. Echo de los argumentos recibidos, para comprobar
 * que el parser los extrae en el orden correcto.
 *
 * Disparadores de error:
 *   ip == "bad"   -> RC_ERR_IP        mask == "bad" -> RC_ERR_MASK
 *   name == "dup" -> RC_ERR_EXISTS    name == "nope" -> RC_ERR_NOT_FOUND
 *   ip == "9.9.9.9" en routes_lookup -> RC_ERR_NO_ROUTE
 */
#include <stdio.h>
#include <string.h>
#include "../src/ipcalc.h"
#include "../src/routes.h"

static int bad(const char *ip, const char *mask)
{
    if (ip && strcmp(ip, "bad") == 0)     return RC_ERR_IP;
    if (mask && strcmp(mask, "bad") == 0) return RC_ERR_MASK;
    return RC_OK;
}

int ipc_parse_ip(const char *s, uint32_t *o)   { (void)s; (void)o; return 1; }
int ipc_parse_mask(const char *s, uint32_t *o) { (void)s; (void)o; return 24; }
void ipc_ip_to_str(uint32_t ip, char *b, size_t n) { (void)ip; snprintf(b, n, "0.0.0.0"); }

int ipc_broadcast(const char *ip, const char *m, char *o, size_t n)
{ int r = bad(ip, m); if (r) return r; snprintf(o, n, "broadcast(%s,%s)", ip, m); return 0; }
int ipc_network(const char *ip, const char *m, char *o, size_t n)
{ int r = bad(ip, m); if (r) return r; snprintf(o, n, "network(%s,%s)", ip, m); return 0; }
int ipc_hosts_range(const char *ip, const char *m, char *o, size_t n)
{ int r = bad(ip, m); if (r) return r; snprintf(o, n, "range(%s,%s)", ip, m); return 0; }
int ipc_netmask(const char *m, char *o, size_t n)
{ int r = bad(NULL, m); if (r) return r; snprintf(o, n, "netmask(%s)", m); return 0; }
int ipc_hosts_count(const char *m, char *o, size_t n)
{ int r = bad(NULL, m); if (r) return r; snprintf(o, n, "count(%s)", m); return 0; }
int ipc_ip_class(const char *ip, char *o, size_t n)
{ int r = bad(ip, NULL); if (r) return r; snprintf(o, n, "class(%s)", ip); return 0; }

int routes_random_subnets(const char *net, const char *m, long num,
                          const char *size, char *o, size_t n)
{ int r = bad(net, m); if (r) return r;
  snprintf(o, n, "random(%s,%s,%ld,%s)", net, m, num, size); return 0; }

int routes_set(const char *name, const char *net, const char *m, long prio)
{ if (strcmp(name, "dup") == 0) return RC_ERR_EXISTS;
  int r = bad(net, m); if (r) return r; (void)prio; return 0; }

int routes_del(const char *name)
{ return strcmp(name, "nope") == 0 ? RC_ERR_NOT_FOUND : RC_OK; }

int routes_lookup(const char *ip, int debug, char *o, size_t n)
{ if (strcmp(ip, "bad") == 0) return RC_ERR_IP;
  if (strcmp(ip, "9.9.9.9") == 0) return RC_ERR_NO_ROUTE;
  snprintf(o, n, "lookup(%s,debug=%d)", ip, debug); return 0; }
