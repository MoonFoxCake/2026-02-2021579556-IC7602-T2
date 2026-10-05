/*
 * server.c - Servidor TCP concurrente (Persona 1)
 *
 * Responsabilidades de este módulo:
 *   1. Sockets:   socket / SO_REUSEADDR / bind(0.0.0.0) / listen / accept.
 *   2. Hilos:     un hilo detached por cliente (la tabla de rutas es global y
 *                 compartida, por eso hilos y no fork()).
 *   3. Líneas:    TCP es un flujo de bytes; aquí se reensamblan las líneas
 *                 (terminan en '\n'), se quita '\r', se filtran las secuencias
 *                 telnet (IAC, 0xFF) y se limita el tamaño de la línea.
 *   4. Señales:   SIGPIPE ignorada; SIGINT/SIGTERM => cierre ordenado
 *                 (Kubernetes envía SIGTERM al detener un Pod).
 *
 * Lo que NO hace: interpretar comandos. Cada línea completa se entrega a
 * parser_process_line() y lo que éste escribe en 'out' se envía tal cual
 * (ya viene terminado en "\r\n"; si viene vacío no se envía nada).
 *
 * Compilar sin main() (para pruebas): -DSERVER_NO_MAIN
 */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include "server.h"
#include "parser.h"

/* ------------------------------------------------------------------ */
/* Constantes                                                          */
/* ------------------------------------------------------------------ */
#define BACKLOG        128   /* cola de conexiones pendientes de accept()   */
#define POLL_MS        250   /* cada cuánto se revisa la bandera de cierre  */
#define SEND_TIMEOUT_S 5     /* un cliente que no lee no bloquea el hilo    */
#define LINGER_MS      200   /* tiempo para que "Bye" llegue antes del close */

/* Bytes del protocolo telnet (RFC 854) */
#define T_IAC   255          /* "Interpret As Command"                      */
#define T_SE    240          /* fin de subnegociación                       */
#define T_SB    250          /* inicio de subnegociación                    */
#define T_WILL  251          /* WILL/WONT/DO/DONT llevan 1 byte de opción   */
#define T_DONT  254

/* ------------------------------------------------------------------ */
/* Estado global (mínimo, todo protegido)                              */
/* ------------------------------------------------------------------ */
/* atomic_int (C11): lo escribe el manejador de señal y lo leen todos los hilos.
 * "volatile sig_atomic_t" sólo es seguro entre manejador e hilo principal; entre
 * hilos es una carrera de datos. Un atómico lock-free es válido en ambos casos. */
static atomic_int g_stop = 0;

static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  g_cv;                  /* se señala cuando g_active baja */
static int             g_active = 0;          /* hilos de cliente vivos      */
static int             g_verbose = 0;

/* ------------------------------------------------------------------ */
/* Bitácora (stdout/stderr sin buffer => visible con docker/kubectl logs) */
/* ------------------------------------------------------------------ */
static void log_msg(FILE *f, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));

static void log_msg(FILE *f, const char *fmt, ...)
{
    char ts[32], msg[768];
    time_t t = time(NULL);
    struct tm tmv;
    localtime_r(&t, &tmv);
    strftime(ts, sizeof ts, "%Y-%m-%d %H:%M:%S", &tmv);

    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);

    fprintf(f, "[%s] %s\n", ts, msg);        /* una sola llamada: no se mezcla */
}

/* Copia 'src' a 'dst' cambiando por '.' lo no imprimible (evita inyectar
 * secuencias de control en la bitácora). */
static void sanitize(const char *src, char *dst, size_t n)
{
    size_t i = 0;
    for (; src[i] && i + 1 < n; i++) {
        unsigned char c = (unsigned char)src[i];
        dst[i] = (c >= 0x20 && c < 0x7f) ? (char)c : '.';
    }
    dst[i] = '\0';
}

/* ------------------------------------------------------------------ */
/* Señales                                                             */
/* ------------------------------------------------------------------ */
static void on_stop_signal(int sig)
{
    (void)sig;
    atomic_store(&g_stop, 1);      /* único trabajo en el manejador (async-signal-safe) */
}

static void install_signals(void)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sigemptyset(&sa.sa_mask);
    sa.sa_handler = on_stop_signal;
    sa.sa_flags   = 0;             /* sin SA_RESTART: poll() devuelve EINTR */
    sigaction(SIGINT,  &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    signal(SIGPIPE, SIG_IGN);      /* escribir a un cliente caído no mata el proceso */
}

/* ------------------------------------------------------------------ */
/* Contador de clientes activos                                        */
/* ------------------------------------------------------------------ */
static int active_try_inc(void)
{
    int ok = 0;
    pthread_mutex_lock(&g_mu);
    if (g_active < SERVER_MAX_CLIENTS) { g_active++; ok = 1; }
    pthread_mutex_unlock(&g_mu);
    return ok;
}

static void active_dec(void)
{
    pthread_mutex_lock(&g_mu);
    g_active--;
    pthread_cond_broadcast(&g_cv);
    pthread_mutex_unlock(&g_mu);
}

/* Espera a que no queden clientes o a que venza 'ms'. Devuelve los que faltan. */
static int active_wait_zero(int ms)
{
    struct timespec dl;
    clock_gettime(CLOCK_MONOTONIC, &dl);
    dl.tv_sec  += ms / 1000;
    dl.tv_nsec += (long)(ms % 1000) * 1000000L;
    if (dl.tv_nsec >= 1000000000L) { dl.tv_sec++; dl.tv_nsec -= 1000000000L; }

    pthread_mutex_lock(&g_mu);
    while (g_active > 0) {
        if (pthread_cond_timedwait(&g_cv, &g_mu, &dl) != 0) break;   /* ETIMEDOUT */
    }
    int left = g_active;
    pthread_mutex_unlock(&g_mu);
    return left;
}

/* ------------------------------------------------------------------ */
/* Escritura y cierre                                                  */
/* ------------------------------------------------------------------ */

/* send() puede escribir menos de lo pedido: se repite hasta enviar todo.
 * MSG_NOSIGNAL evita SIGPIPE por socket (defensa adicional a SIG_IGN). */
static int send_all(int fd, const char *buf, size_t len)
{
    size_t off = 0;
    while (off < len) {
        ssize_t n = send(fd, buf + off, len - off, MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;                       /* EPIPE, ECONNRESET, timeout... */
        }
        off += (size_t)n;
    }
    return 0;
}

/* Envía una cadena C completa (la longitud se calcula con strlen: los mensajes
 * llevan tildes y en UTF-8 ocupan más de un byte). */
static int send_str(int fd, const char *s)
{
    return send_all(fd, s, strlen(s));
}

/* Cierre ordenado: se avisa FIN, se drenan unos ms lo que el cliente aún
 * envíe y recién entonces close(). Cerrar con datos sin leer provoca un RST
 * que puede borrar el "Bye" antes de que el cliente lo lea. */
static void graceful_close(int fd)
{
    shutdown(fd, SHUT_WR);
    if (!atomic_load(&g_stop)) {
        char tmp[256];
        struct pollfd p = { .fd = fd, .events = POLLIN, .revents = 0 };
        for (int waited = 0; waited < LINGER_MS; waited += 50) {
            int pr = poll(&p, 1, 50);
            if (pr < 0 && errno != EINTR) break;
            if (pr > 0 && recv(fd, tmp, sizeof tmp, 0) <= 0) break;
        }
    }
    close(fd);
}

/* ------------------------------------------------------------------ */
/* Reensamblado de líneas + filtro telnet                              */
/* ------------------------------------------------------------------ */
typedef enum {
    S_DATA,      /* bytes normales                                      */
    S_IAC,       /* se vio IAC; falta el byte de comando                */
    S_OPT,       /* WILL/WONT/DO/DONT; falta el byte de opción          */
    S_SB,        /* dentro de una subnegociación (IAC SB ... IAC SE)    */
    S_SB_IAC     /* dentro de SB se vio IAC; ¿viene SE?                 */
} tstate_t;

typedef struct {
    char     line[MAX_LINE + 1];   /* contenido acumulado, sin \r ni \n    */
    size_t   len;                  /* bytes válidos en line                */
    tstate_t st;                   /* estado del filtro (persiste entre recv) */
} linebuf_t;

/* Guarda un byte de la línea. Si ya hay MAX_LINE bytes, descarta el resto
 * pero deja len == MAX_LINE: el parser interpreta eso como "línea demasiado
 * larga" y responde con su propio mensaje de error. */
static void lb_put(linebuf_t *lb, unsigned char c)
{
    if (lb->len < MAX_LINE) lb->line[lb->len++] = (char)c;
}

/* Procesa un byte. Devuelve 1 cuando 'c' cerró una línea ('\n'). */
static int lb_feed(linebuf_t *lb, unsigned char c)
{
    switch (lb->st) {
    case S_IAC:
        if (c == T_IAC) {                       /* IAC IAC = el byte 0xFF literal */
            lb->st = S_DATA;
            lb_put(lb, c);                      /* el parser lo rechazará */
        } else if (c == T_SB) {
            lb->st = S_SB;
        } else if (c >= T_WILL && c <= T_DONT) {
            lb->st = S_OPT;
        } else {
            lb->st = S_DATA;                    /* comando de 2 bytes (NOP, IP...) */
        }
        return 0;

    case S_OPT:
        lb->st = S_DATA;                        /* se descarta el byte de opción */
        return 0;

    case S_SB:
        if (c == T_IAC) lb->st = S_SB_IAC;
        return 0;

    case S_SB_IAC:
        lb->st = (c == T_SE) ? S_DATA : S_SB;   /* IAC SE termina; IAC IAC sigue */
        return 0;

    case S_DATA:
    default:
        if (c == T_IAC)  { lb->st = S_IAC; return 0; }
        if (c == '\n')   return 1;
        if (c == '\r' || c == '\0') return 0;   /* telnet envía "\r\n" o "\r\0" */
        lb_put(lb, c);
        return 0;
    }
}

/* ------------------------------------------------------------------ */
/* Atención de un cliente                                              */
/* ------------------------------------------------------------------ */
typedef struct {
    int  fd;
    char peer[64];                 /* "a.b.c.d:puerto", para la bitácora */
} client_t;

/* Entrega la línea acumulada al parser y envía la respuesta.
 * Devuelve -1 si hay que cerrar la conexión (EXIT/QUIT o error de envío). */
static int dispatch_line(const client_t *c, linebuf_t *lb)
{
    char out[PARSER_OUT_MAX];

    lb->line[lb->len] = '\0';
    if (g_verbose && lb->len > 0) {
        char clean[MAX_LINE + 1];
        sanitize(lb->line, clean, sizeof clean);
        log_msg(stdout, "%s > %s", c->peer, clean);
    }

    int r = parser_process_line(lb->line, out, sizeof out);
    lb->len = 0;

    if (out[0] != '\0' && send_all(c->fd, out, strlen(out)) < 0) return -1;
    return (r == PARSER_QUIT) ? -1 : 0;
}

static void *client_thread(void *arg)
{
    client_t *c = arg;
    linebuf_t lb;
    unsigned char buf[1024];
    memset(&lb, 0, sizeof lb);

    log_msg(stdout, "conexión abierta: %s", c->peer);

    while (!atomic_load(&g_stop)) {
        /* poll con tiempo límite: así el hilo se entera del cierre del servidor */
        struct pollfd p = { .fd = c->fd, .events = POLLIN, .revents = 0 };
        int pr = poll(&p, 1, POLL_MS);
        if (pr < 0) { if (errno == EINTR) continue; break; }
        if (pr == 0) continue;

        ssize_t n = recv(c->fd, buf, sizeof buf, 0);
        if (n < 0) { if (errno == EINTR) continue; break; }   /* ECONNRESET... */

        if (n == 0) {                       /* el cliente cerró su lado */
            if (lb.len > 0) dispatch_line(c, &lb);   /* última línea sin '\n' */
            break;
        }

        int stop = 0;
        for (ssize_t i = 0; i < n && !stop; i++)
            if (lb_feed(&lb, buf[i]) && dispatch_line(c, &lb) < 0) stop = 1;
        if (stop) break;
    }

    if (atomic_load(&g_stop))
        send_str(c->fd, "ERROR: el servidor se está deteniendo\r\n");

    graceful_close(c->fd);
    log_msg(stdout, "conexión cerrada: %s", c->peer);
    free(c);
    active_dec();
    return NULL;
}

/* ------------------------------------------------------------------ */
/* Socket de escucha                                                   */
/* ------------------------------------------------------------------ */
static int open_listener(unsigned short port)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { log_msg(stderr, "socket: %s", strerror(errno)); return -1; }

    int one = 1;                    /* reiniciar sin "Address already in use" */
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);

    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family      = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_ANY);   /* 0.0.0.0: obligatorio en contenedores */
    a.sin_port        = htons(port);

    if (bind(fd, (struct sockaddr *)&a, sizeof a) < 0) {
        log_msg(stderr, "bind (puerto %u): %s", port, strerror(errno));
        close(fd);
        return -1;
    }
    if (listen(fd, BACKLOG) < 0) {
        log_msg(stderr, "listen: %s", strerror(errno));
        close(fd);
        return -1;
    }
    /* No bloqueante: tras poll() un accept() nunca se queda colgado si el
     * cliente aborta justo en medio. */
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
    return fd;
}

/* Opciones de cada conexión aceptada. */
static void tune_client_socket(int fd)
{
    int one = 1;
    struct timeval tv = { .tv_sec = SEND_TIMEOUT_S, .tv_usec = 0 };

    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) & ~O_NONBLOCK);  /* blocking (algunos SO lo heredan) */
    setsockopt(fd, SOL_SOCKET,  SO_KEEPALIVE, &one, sizeof one);  /* detecta pares muertos */
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY,  &one, sizeof one);  /* respuestas cortas sin demora */
    setsockopt(fd, SOL_SOCKET,  SO_SNDTIMEO,  &tv,  sizeof tv);   /* send() no bloquea para siempre */
}

/* Crea el hilo de un cliente. Devuelve 0 si quedó en marcha. */
static int spawn_client(int fd, const struct sockaddr_in *sa)
{
    client_t *c = malloc(sizeof *c);
    if (!c) return -1;
    c->fd = fd;

    char ip[INET_ADDRSTRLEN] = "?";
    inet_ntop(AF_INET, &sa->sin_addr, ip, sizeof ip);
    snprintf(c->peer, sizeof c->peer, "%s:%u", ip, (unsigned)ntohs(sa->sin_port));

    pthread_attr_t at;
    pthread_attr_init(&at);
    pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);  /* nadie hace join */

    pthread_t tid;
    int rc = pthread_create(&tid, &at, client_thread, c);
    pthread_attr_destroy(&at);
    if (rc != 0) { free(c); return -1; }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Ciclo principal                                                     */
/* ------------------------------------------------------------------ */
int server_run(unsigned short port, int verbose)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);

    g_verbose = verbose;
    atomic_store(&g_stop, 0);
    g_active  = 0;

    pthread_condattr_t ca;                    /* espera con reloj monotónico */
    pthread_condattr_init(&ca);
    pthread_condattr_setclock(&ca, CLOCK_MONOTONIC);
    pthread_cond_init(&g_cv, &ca);
    pthread_condattr_destroy(&ca);

    install_signals();

    int lfd = open_listener(port);
    if (lfd < 0) return 1;
    log_msg(stdout, "servidor escuchando en 0.0.0.0:%u (máx. %d clientes)",
            port, SERVER_MAX_CLIENTS);

    while (!atomic_load(&g_stop)) {
        struct pollfd p = { .fd = lfd, .events = POLLIN, .revents = 0 };
        int pr = poll(&p, 1, POLL_MS);
        if (pr < 0) {
            if (errno == EINTR) continue;     /* llegó una señal: se re-evalúa g_stop */
            log_msg(stderr, "poll: %s", strerror(errno));
            break;
        }
        if (pr == 0) continue;

        struct sockaddr_in ca_addr;
        socklen_t cl = sizeof ca_addr;
        int cfd = accept(lfd, (struct sockaddr *)&ca_addr, &cl);
        if (cfd < 0) {
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK ||
                errno == ECONNABORTED) continue;
            log_msg(stderr, "accept: %s", strerror(errno));
            if (errno == EMFILE || errno == ENFILE) usleep(100 * 1000);  /* sin descriptores */
            continue;
        }

        tune_client_socket(cfd);

        if (!active_try_inc()) {              /* servidor lleno */
            send_str(cfd, "ERROR: servidor lleno, intente más tarde\r\n");
            log_msg(stderr, "conexión rechazada (límite de %d clientes)",
                    SERVER_MAX_CLIENTS);
            close(cfd);
            continue;
        }
        if (spawn_client(cfd, &ca_addr) != 0) {
            log_msg(stderr, "no se pudo crear el hilo del cliente");
            active_dec();
            close(cfd);
        }
    }

    /* ---- cierre ordenado ---- */
    log_msg(stdout, "señal de cierre recibida: dejando de aceptar conexiones");
    close(lfd);
    int left = active_wait_zero(SERVER_SHUTDOWN_GRACE_MS);
    if (left > 0)
        log_msg(stderr, "cierre forzado: %d cliente(s) aún activos", left);
    log_msg(stdout, "servidor detenido");
    return 0;
}

/* ------------------------------------------------------------------ */
/* main()                                                              */
/* ------------------------------------------------------------------ */
#ifndef SERVER_NO_MAIN
#include <getopt.h>

static void usage(const char *prog)
{
    fprintf(stderr,
            "Uso: %s [-p puerto] [-v]\n"
            "  -p puerto   puerto TCP de escucha (por defecto %d)\n"
            "  -v          bitácora detallada: registra cada comando recibido\n",
            prog, SERVER_DEFAULT_PORT);
}

int main(int argc, char **argv)
{
    long port = SERVER_DEFAULT_PORT;
    int  verbose = 0, opt;

    while ((opt = getopt(argc, argv, "p:vh")) != -1) {
        switch (opt) {
        case 'p': {
            char *end = NULL;
            errno = 0;
            port = strtol(optarg, &end, 10);
            if (errno != 0 || *end != '\0' || port < 1 || port > 65535) {
                fprintf(stderr, "Puerto inválido: '%s' (use 1-65535)\n", optarg);
                return 2;
            }
            break;
        }
        case 'v': verbose = 1; break;
        case 'h': usage(argv[0]); return 0;
        default:  usage(argv[0]); return 2;
        }
    }
    return server_run((unsigned short)port, verbose);
}
#endif /* SERVER_NO_MAIN */