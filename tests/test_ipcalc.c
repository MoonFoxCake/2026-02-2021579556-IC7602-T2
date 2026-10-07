/* Pruebas unitarias de ipcalc.c con los ejemplos del enunciado. */
#include <stdio.h>
#include <string.h>

#include "../src/ipcalc.h"

static int failures, checks;

#define CHECK(condition, message) do { \
    checks++; \
    if (!(condition)) { \
        failures++; \
        printf("FAIL: %s (linea %d)\n", message, __LINE__); \
    } \
} while (0)

/* Verifica que la llamada devuelva RC_OK y escriba exactamente 'want'. */
#define EXPECT(call, want) do { \
    char out[128] = ""; \
    int rc_ = (call); \
    CHECK(rc_ == RC_OK && strcmp(out, want) == 0, #call " -> " want); \
    if (rc_ != RC_OK || strcmp(out, want) != 0) \
        printf("      obtenido rc=%d out=\"%s\"\n", rc_, out); \
} while (0)

#define EXPECT_RC(call, want_rc) do { \
    char out[128] = ""; \
    (void)out; \
    CHECK((call) == (want_rc), #call " -> " #want_rc); \
} while (0)

int main(void)
{
    /* 1. Broadcast */
    EXPECT(ipc_broadcast("10.8.2.5", "/29", out, sizeof out), "10.8.2.7");
    EXPECT(ipc_broadcast("172.16.0.56", "255.255.255.128", out, sizeof out),
           "172.16.0.127");

    /* 2. Número de red */
    EXPECT(ipc_network("10.8.2.5", "/29", out, sizeof out), "10.8.2.0");
    EXPECT(ipc_network("172.16.0.56", "255.255.255.128", out, sizeof out),
           "172.16.0.0");

    /* 3. Rango de hosts */
    EXPECT(ipc_hosts_range("10.8.2.5", "/29", out, sizeof out), "10.8.2.{1-6}");
    EXPECT(ipc_hosts_range("172.16.0.56", "255.255.255.128", out, sizeof out),
           "172.16.0.{1-126}");
    EXPECT(ipc_hosts_range("172.16.3.4", "/16", out, sizeof out),
           "172.16.{0.1-255.254}");
    EXPECT(ipc_hosts_range("10.0.0.4", "/31", out, sizeof out), "10.0.0.{4-5}");
    EXPECT(ipc_hosts_range("10.0.0.4", "/32", out, sizeof out), "10.0.0.4");

    /* 4. Máscara / prefijo */
    EXPECT(ipc_netmask("/27", out, sizeof out), "255.255.255.224");
    EXPECT(ipc_netmask("255.255.255.0", out, sizeof out), "/24");
    EXPECT(ipc_netmask("/0", out, sizeof out), "0.0.0.0");

    /* 5. Cantidad de hosts */
    EXPECT(ipc_hosts_count("/29", out, sizeof out), "6");
    EXPECT(ipc_hosts_count("255.255.255.128", out, sizeof out), "126");
    EXPECT(ipc_hosts_count("/0", out, sizeof out), "4294967294");
    EXPECT(ipc_hosts_count("/32", out, sizeof out), "1");

    /* 6. Clase */
    EXPECT(ipc_ip_class("10.8.2.5", out, sizeof out), "Clase A");
    EXPECT(ipc_ip_class("172.16.0.1", out, sizeof out), "Clase B");
    EXPECT(ipc_ip_class("192.168.1.1", out, sizeof out), "Clase C");
    EXPECT(ipc_ip_class("224.0.0.1", out, sizeof out), "Clase D");
    EXPECT(ipc_ip_class("250.0.0.1", out, sizeof out), "Clase E");

    /* Errores */
    EXPECT_RC(ipc_broadcast("256.1.1.1", "/24", out, sizeof out), RC_ERR_IP);
    EXPECT_RC(ipc_broadcast("1.2.3.4abc", "/24", out, sizeof out), RC_ERR_IP);
    EXPECT_RC(ipc_broadcast("1.2.3", "/24", out, sizeof out), RC_ERR_IP);
    EXPECT_RC(ipc_network("1.2.3.4", "/33", out, sizeof out), RC_ERR_MASK);
    EXPECT_RC(ipc_network("1.2.3.4", "24", out, sizeof out), RC_ERR_MASK);
    EXPECT_RC(ipc_network("1.2.3.4", "/", out, sizeof out), RC_ERR_MASK);
    EXPECT_RC(ipc_netmask("255.0.255.0", out, sizeof out), RC_ERR_MASK);
    EXPECT_RC(ipc_hosts_count("/-1", out, sizeof out), RC_ERR_MASK);
    EXPECT_RC(ipc_ip_class("hola", out, sizeof out), RC_ERR_IP);
    EXPECT_RC(ipc_broadcast("10.8.2.5", "/29", out, 4), RC_ERR_INTERNAL);

    printf("ipcalc: %d verificaciones, %d fallos\n", checks, failures);
    return failures ? 1 : 0;
}
