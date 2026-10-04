CC      ?= gcc
CFLAGS  := -std=gnu11 -Wall -Wextra -Wpedantic -O2 -g
LDFLAGS := -pthread
BUILD   := build

.PHONY: all server test test-parser clean

all: server

# Servidor completo (requiere server.c, ipcalc.c y routes.c de los demás)
server: $(BUILD)/server
$(BUILD)/server: src/server.c src/parser.c src/ipcalc.c src/routes.c | $(BUILD)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

# Pruebas del parser, aisladas (usan tests/stubs.c en lugar de ipcalc/routes)
test-parser: $(BUILD)/test_parser
	./$(BUILD)/test_parser
$(BUILD)/test_parser: tests/test_parser.c tests/stubs.c src/parser.c | $(BUILD)
	$(CC) $(CFLAGS) -o $@ $^

test: test-parser

$(BUILD):
	mkdir -p $(BUILD)

clean:
	rm -rf $(BUILD)
