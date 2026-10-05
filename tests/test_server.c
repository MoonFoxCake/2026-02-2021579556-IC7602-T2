/*
 * test_server.c - Pruebas de integración del servidor (Persona 1).
 *
 * Levanta server_run() en un proceso hijo (fork) y lo ataca como lo haría
 * telnet: por un socket TCP real. Usa tests/stubs.c en lugar de ipcalc/routes,
 * así que prueba SOLO sockets, hilos, lectura de líneas y señales.
 *
 * Compilar:  make test-server
 */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include "../src/server.h"
#include "../src/parser.h"

static int g_port;
static int g_fails = 0, g_checks = 0;

#define CHECK(cond, name) do {                                  \
    g_checks++;                                                 \
    if (!(cond)) { g_fails++; printf("FAIL: %s\n", name); }     \
} while (0)

/* ---------------- utilidades de cliente ---------------- */
static int dial(void)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons((uint16_t)g_port) };
    inet_pton(AF_INET, "127.0.0.1", &a.sin_addr);
    if (connect(fd, (struct sockaddr *)&a, sizeof a) < 0) { close(fd); return -1; }
    return fd;
}

static int dial_retry(int ms)
{
    for (int t = 0; t < ms; t += 50) {
        int fd = dial();
        if (fd >= 0) return fd;
        usleep(50 * 1000);
    }
    return -1;
}

static void send_str(int fd, const char *s) { send(fd, s, strlen(s), MSG_NOSIGNAL); }

/* Lee hasta ver 'needle', EOF o agotar 'ms'. Devuelve bytes leídos. */
static int recv_until(int fd, char *buf, size_t n, const char *needle, int ms)
{
    size_t got = 0;
    buf[0] = '\0';
    struct timeval tv = { .tv_sec = ms / 1000, .tv_usec = (ms % 1000) * 1000 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    while (got + 1 < n) {
        ssize_t r = recv(fd, buf + got, n - 1 - got, 0);
        if (r <= 0) break;
        got += (size_t)r;
        buf[got] = '\0';
        if (needle && strstr(buf, needle)) break;
    }
    return (int)got;
}

/* ¿La conexión llegó a EOF (recv == 0) dentro de 'ms'? */
static int at_eof(int fd, int ms)
{
    char b[256];
    struct timeval tv = { .tv_sec = ms / 1000, .tv_usec = (ms % 1000) * 1000 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    for (;;) {
        ssize_t r = recv(fd, b, sizeof b, 0);
        if (r == 0) return 1;
        if (r < 0) return 0;
    }
}

/* ---------------- hilos clientes concurrentes ---------------- */
#define NTHREADS 32
static int g_thr_ok[NTHREADS];

static void *worker(void *arg)
{
    int id = (int)(long)arg, ok = 1;
    int fd = dial_retry(1000);
    if (fd < 0) { g_thr_ok[id] = 0; return NULL; }
    for (int k = 0; k < 5 && ok; k++) {
        char cmd[96], want[64], got[256];
        snprintf(cmd,  sizeof cmd,  "GET IP CLASS 10.%d.0.%d\r\n", id, k);
        snprintf(want, sizeof want, "class(10.%d.0.%d)", id, k);
        send_str(fd, cmd);
        recv_until(fd, got, sizeof got, "\r\n", 2000);
        ok = strstr(got, want) != NULL;
    }
    close(fd);
    g_thr_ok[id] = ok;
    return NULL;
}

/* ---------------- main ---------------- */
int main(void)
{
    /* Puerto libre: se pide uno efímero y se libera. */
    int tmp = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK) };
    socklen_t al = sizeof a;
    bind(tmp, (struct sockaddr *)&a, sizeof a);
    getsockname(tmp, (struct sockaddr *)&a, &al);
    g_port = ntohs(a.sin_port);
    close(tmp);

    pid_t pid = fork();
    if (pid == 0) {                               /* hijo = servidor */
        if (!freopen("/dev/null", "w", stdout)) _exit(3);
        if (!freopen("/dev/null", "w", stderr)) _exit(3);
        _exit(server_run((unsigned short)g_port, 0));
    }

    int probe = dial_retry(3000);
    CHECK(probe >= 0, "el servidor acepta conexiones");
    if (probe < 0) { kill(pid, SIGKILL); return 1; }
    close(probe);

    char buf[PARSER_OUT_MAX];
    int fd;

    /* 1. Comando simple */
    fd = dial();
    send_str(fd, "GET IP CLASS 10.8.2.5\r\n");
    recv_until(fd, buf, sizeof buf, "\r\n", 2000);
    CHECK(strcmp(buf, "class(10.8.2.5)\r\n") == 0, "1. comando simple, respuesta con CRLF");

    /* 2. Mayúsculas/minúsculas y solo '\n' (nc, no telnet) */
    send_str(fd, "get ip class 1.2.3.4\n");
    recv_until(fd, buf, sizeof buf, "\r\n", 2000);
    CHECK(strstr(buf, "class(1.2.3.4)") != NULL, "2. línea terminada solo en \\n");

    /* 3. Línea fragmentada byte a byte (TCP no preserva fronteras) */
    const char *frag = "GET IP CLASS 9.9.9.1\r\n";
    for (size_t i = 0; frag[i]; i++) { send(fd, frag + i, 1, MSG_NOSIGNAL); usleep(2000); }
    recv_until(fd, buf, sizeof buf, "\r\n", 2000);
    CHECK(strstr(buf, "class(9.9.9.1)") != NULL, "3. línea fragmentada byte a byte");

    /* 4. Dos comandos en un solo send (pipelining) => dos respuestas */
    send_str(fd, "GET IP CLASS 1.1.1.1\r\nGET IP CLASS 2.2.2.2\r\n");
    recv_until(fd, buf, sizeof buf, "class(2.2.2.2)", 2000);
    CHECK(strstr(buf, "class(1.1.1.1)") && strstr(buf, "class(2.2.2.2)") &&
          strstr(buf, "class(1.1.1.1)") < strstr(buf, "class(2.2.2.2)"),
          "4. dos comandos en un paquete, respuestas en orden");

    /* 5. Línea vacía: sin respuesta; la conexión sigue sirviendo */
    send_str(fd, "\r\n\r\nGET IP CLASS 3.3.3.3\r\n");
    recv_until(fd, buf, sizeof buf, "\r\n", 2000);
    CHECK(strcmp(buf, "class(3.3.3.3)\r\n") == 0, "5. líneas vacías no generan respuesta");

    /* 6. Negociación telnet (IAC WILL ECHO, IAC DO SGA, SB ... SE) se ignora */
    send(fd, "\xff\xfb\x01\xff\xfd\x03\xff\xfa\x18\x00\xff\xf0", 12, MSG_NOSIGNAL);
    send_str(fd, "GET IP CLASS 4.4.4.4\r\n");
    recv_until(fd, buf, sizeof buf, "\r\n", 2000);
    CHECK(strcmp(buf, "class(4.4.4.4)\r\n") == 0, "6. secuencias IAC de telnet ignoradas");

    /* 7. "\r\0" (telnet en modo carácter) */
    send(fd, "GET IP CLASS 5.5.5.5\r\0\n", 23, MSG_NOSIGNAL);
    recv_until(fd, buf, sizeof buf, "\r\n", 2000);
    CHECK(strstr(buf, "class(5.5.5.5)") != NULL, "7. \\r\\0 se tolera");

    /* 8. Línea gigante: error del parser y la conexión sigue viva */
    char *big = malloc(4001);
    memset(big, 'A', 4000); big[4000] = '\0';
    send_str(fd, big); send_str(fd, "\r\n");
    free(big);
    recv_until(fd, buf, sizeof buf, "\r\n", 2000);
    CHECK(strstr(buf, "demasiado larga") != NULL, "8. línea de 4000 bytes => ERROR");
    send_str(fd, "GET IP CLASS 6.6.6.6\r\n");
    recv_until(fd, buf, sizeof buf, "\r\n", 2000);
    CHECK(strcmp(buf, "class(6.6.6.6)\r\n") == 0, "8b. tras la línea gigante el servidor sigue sincronizado");

    /* 9. Última línea sin '\n' + cierre de escritura (printf | nc) */
    send_str(fd, "GET IP CLASS 7.7.7.7");
    shutdown(fd, SHUT_WR);
    recv_until(fd, buf, sizeof buf, "class(7.7.7.7)", 2000);
    CHECK(strstr(buf, "class(7.7.7.7)") != NULL, "9. línea final sin \\n se procesa en EOF");
    CHECK(at_eof(fd, 2000), "9b. el servidor cierra tras el EOF del cliente");
    close(fd);

    /* 10. EXIT: "Bye" y cierre */
    fd = dial();
    send_str(fd, "EXIT\r\n");
    recv_until(fd, buf, sizeof buf, "Bye", 2000);
    CHECK(strstr(buf, "Bye") != NULL, "10. EXIT responde Bye");
    CHECK(at_eof(fd, 2000), "10b. EXIT cierra la conexión");
    close(fd);

    /* 11. Cliente que se va con RST sin leer: el servidor no muere (SIGPIPE) */
    for (int i = 0; i < 5; i++) {
        fd = dial();
        send_str(fd, "GET IP CLASS 8.8.8.8\r\nGET IP CLASS 8.8.8.8\r\n");
        struct linger lg = { .l_onoff = 1, .l_linger = 0 };
        setsockopt(fd, SOL_SOCKET, SO_LINGER, &lg, sizeof lg);
        close(fd);                                  /* => RST */
    }
    usleep(200 * 1000);
    CHECK(waitpid(pid, NULL, WNOHANG) == 0, "11. el servidor sobrevive a clientes caídos (SIGPIPE)");
    fd = dial();
    send_str(fd, "GET IP CLASS 8.8.4.4\r\n");
    recv_until(fd, buf, sizeof buf, "\r\n", 2000);
    CHECK(strstr(buf, "class(8.8.4.4)") != NULL, "11b. sigue atendiendo clientes nuevos");
    close(fd);

    /* 12. Concurrencia: 32 clientes en paralelo, 5 comandos cada uno */
    pthread_t th[NTHREADS];
    for (long i = 0; i < NTHREADS; i++) pthread_create(&th[i], NULL, worker, (void *)i);
    for (int i = 0; i < NTHREADS; i++) pthread_join(th[i], NULL);
    int all = 1;
    for (int i = 0; i < NTHREADS; i++) all = all && g_thr_ok[i];
    CHECK(all, "12. 32 clientes concurrentes reciben sus propias respuestas");

    /* 13. Límite de clientes: el (MAX+1)-ésimo es rechazado con mensaje */
    usleep(300 * 1000);                              /* que se liberen los anteriores */
    static int held[SERVER_MAX_CLIENTS];
    int nheld = 0;
    for (; nheld < SERVER_MAX_CLIENTS; nheld++) {
        held[nheld] = dial();
        if (held[nheld] < 0) break;
    }
    usleep(300 * 1000);
    fd = dial();
    recv_until(fd, buf, sizeof buf, "lleno", 2000);
    CHECK(nheld == SERVER_MAX_CLIENTS && strstr(buf, "servidor lleno") != NULL,
          "13. conexión número MAX+1 rechazada con 'servidor lleno'");
    close(fd);

    /* 14. SIGTERM con clientes conectados: aviso, cierre ordenado, exit 0 */
    kill(pid, SIGTERM);
    int status = -1, done = 0;
    for (int t = 0; t < 100 && !done; t++) {         /* hasta 5 s */
        if (waitpid(pid, &status, WNOHANG) == pid) done = 1;
        else usleep(50 * 1000);
    }
    CHECK(done, "14. SIGTERM detiene el servidor en < 5 s");
    CHECK(done && WIFEXITED(status) && WEXITSTATUS(status) == 0, "14b. código de salida 0");
    if (!done) kill(pid, SIGKILL);
    CHECK(at_eof(held[0], 1000), "14c. los clientes conectados reciben el cierre");
    for (int i = 0; i < nheld; i++) close(held[i]);

    printf("%d verificaciones, %d fallos\n", g_checks, g_fails);
    return g_fails ? 1 : 0;
}