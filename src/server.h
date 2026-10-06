/*
 * server.h - Interfaz del servidor TCP (Persona 1)
 *
 * El servidor escucha en 0.0.0.0:<puerto>, atiende cada cliente en un hilo
 * propio y delega cada línea recibida a parser_process_line() (parser.h).
 *
 *   server_run() BLOQUEA hasta que llegue SIGINT/SIGTERM. Al recibir la señal
 *   deja de aceptar conexiones, avisa a los clientes, espera (máx.
 *   SERVER_SHUTDOWN_GRACE_MS) a que sus hilos terminen y retorna.
 *
 * Retorna 0 si terminó limpiamente, 1 si no pudo iniciar (bind, listen...).
 *
 * Línea de comandos del ejecutable (ver main() en server.c):
 *     server [-p puerto] [-v]
 */
#ifndef SERVER_H
#define SERVER_H

#define SERVER_DEFAULT_PORT       9666   /* puerto del enunciado               */
#define SERVER_MAX_CLIENTS        128    /* conexiones simultáneas máximas     */
#define SERVER_SHUTDOWN_GRACE_MS  3000   /* espera máxima al cerrar el servidor */

int server_run(unsigned short port, int verbose);

#endif /* SERVER_H */