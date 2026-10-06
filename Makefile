CC      ?= gcc
CFLAGS  := -std=gnu11 -Wall -Wextra -Wpedantic -O2 -g
LDFLAGS := -pthread
BUILD   := build

.PHONY: all server server-stub test test-parser test-routes test-server clean

all: server

# Servidor completo (requiere server.c, parser.c, ipcalc.c y routes.c)
server: $(BUILD)/server
$(BUILD)/server: src/server.c src/parser.c src/ipcalc.c src/routes.c | $(BUILD)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

# Servidor con ipcalc/routes FALSOS (tests/stubs.c): permite probar sockets,
# hilos y parser con telnet sin esperar a las Personas 3 y 4.
#   make server-stub && ./build/server_stub -v -p 9666
server-stub: $(BUILD)/server_stub
$(BUILD)/server_stub: src/server.c src/parser.c tests/stubs.c | $(BUILD)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

# Pruebas del parser, aisladas (usan tests/stubs.c en lugar de ipcalc/routes)
test-parser: $(BUILD)/test_parser
	./$(BUILD)/test_parser
$(BUILD)/test_parser: tests/test_parser.c tests/stubs.c src/parser.c | $(BUILD)
	$(CC) $(CFLAGS) -o $@ $^

# Pruebas de integración del servidor (sockets reales + stubs)
test-routes: $(BUILD)/test_routes
	./$(BUILD)/test_routes
$(BUILD)/test_routes: tests/test_routes.c src/routes.c | $(BUILD)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

test-server: $(BUILD)/test_server
	./$(BUILD)/test_server
$(BUILD)/test_server: src/test_server.c src/server.c src/parser.c tests/stubs.c | $(BUILD)
	$(CC) $(CFLAGS) -DSERVER_NO_MAIN -o $@ $^ $(LDFLAGS)

test: test-parser test-routes test-server

$(BUILD):
	mkdir -p $(BUILD)

clean:
	rm -rf $(BUILD)
