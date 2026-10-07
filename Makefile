CC      := gcc
CFLAGS  := -std=gnu11 -Wall -Wextra -Wpedantic -O2 -g -Isrc
LDFLAGS := -pthread
BUILD   := build
# Si cambia cualquier cabecera, se recompila todo
HDRS    := $(wildcard src/*.h)
# Solo los .c de los prerrequisitos van a gcc (los .h están para dependencias)
SRCS     = $(filter %.c,$^)

.PHONY: all server server-stub test test-parser test-routes test-ipcalc test-server clean

all: server

# Servidor completo (server.c, parser.c, ipcalc.c y routes.c)
#   make && ./build/server -v -p 9666
server: $(BUILD)/server
$(BUILD)/server: src/server.c src/parser.c src/ipcalc.c src/routes.c $(HDRS) | $(BUILD)
	$(CC) $(CFLAGS) -o $@ $(SRCS) $(LDFLAGS)

# Servidor con ipcalc/routes FALSOS (tests/stubs.c): permite probar sockets,
# hilos y parser con telnet de forma aislada.
#   make server-stub && ./build/server_stub -v -p 9666
server-stub: $(BUILD)/server_stub
$(BUILD)/server_stub: src/server.c src/parser.c tests/stubs.c $(HDRS) | $(BUILD)
	$(CC) $(CFLAGS) -o $@ $(SRCS) $(LDFLAGS)

# Pruebas del parser, aisladas (usan tests/stubs.c en lugar de ipcalc/routes)
test-parser: $(BUILD)/test_parser
	./$(BUILD)/test_parser
$(BUILD)/test_parser: tests/test_parser.c tests/stubs.c src/parser.c $(HDRS) | $(BUILD)
	$(CC) $(CFLAGS) -o $@ $(SRCS)

# Pruebas unitarias de routes.c (subredes aleatorias y tabla de rutas)
test-routes: $(BUILD)/test_routes
	./$(BUILD)/test_routes
$(BUILD)/test_routes: tests/test_routes.c src/routes.c $(HDRS) | $(BUILD)
	$(CC) $(CFLAGS) -o $@ $(SRCS) $(LDFLAGS)

# Pruebas unitarias de ipcalc.c (ejemplos del enunciado y errores)
test-ipcalc: $(BUILD)/test_ipcalc
	./$(BUILD)/test_ipcalc
$(BUILD)/test_ipcalc: tests/test_ipcalc.c src/ipcalc.c $(HDRS) | $(BUILD)
	$(CC) $(CFLAGS) -o $@ $(SRCS)

# Pruebas de integración del servidor (sockets reales + stubs)
test-server: $(BUILD)/test_server
	./$(BUILD)/test_server
$(BUILD)/test_server: tests/test_server.c src/server.c src/parser.c tests/stubs.c $(HDRS) | $(BUILD)
	$(CC) $(CFLAGS) -DSERVER_NO_MAIN -o $@ $(SRCS) $(LDFLAGS)

test: test-parser test-routes test-ipcalc test-server

$(BUILD):
	mkdir -p $(BUILD)

clean:
	rm -rf $(BUILD)
