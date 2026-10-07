#!/usr/bin/env bash
# ==============================================================================
# test_connection.sh - Prueba automatizada de comunicación con el servidor
#
# Se conecta mediante TCP al servidor desplegado y envía comandos de prueba
# para validar las respuestas de las 10 primitivas.
#
# Uso:
#   ./test_connection.sh [HOST] [PUERTO]
# Ejemplo:
#   ./test_connection.sh localhost 9666
#   ./test_connection.sh $(minikube ip) 30966
# ==============================================================================

set -euo pipefail

HOST="${1:-}"
PORT="${2:-}"

# Si no se especifican argumentos, intentar autodetectar
if [ -z "$HOST" ] || [ -z "$PORT" ]; then
    if command -v minikube >/dev/null 2>&1 && minikube status >/dev/null 2>&1; then
        HOST=$(minikube ip)
        PORT="30966"
    else
        HOST="127.0.0.1"
        PORT="9666"
    fi
fi

echo "Probando conexión con el servidor en ${HOST}:${PORT}..."

# Función para enviar un comando y capturar la respuesta
send_command() {
    local cmd="$1"
    if command -v nc >/dev/null 2>&1; then
        printf "%s\r\nEXIT\r\n" "$cmd" | nc -w 3 "$HOST" "$PORT" | head -n 1
    else
        exec 3<>/dev/tcp/"$HOST"/"$PORT"
        echo -ne "${cmd}\r\nEXIT\r\n" >&3
        local resp
        read -r resp <&3
        exec 3<&-
        exec 3>&-
        echo "$resp"
    fi
}

echo "1. GET BROADCAST IP 10.8.2.5 MASK /29"
RESP=$(send_command "GET BROADCAST IP 10.8.2.5 MASK /29")
echo "   Respuesta: $RESP"

echo "2. GET NETWORK NUMBER IP 172.16.0.56 MASK 255.255.255.128"
RESP=$(send_command "GET NETWORK NUMBER IP 172.16.0.56 MASK 255.255.255.128")
echo "   Respuesta: $RESP"

echo "3. GET HOSTS RANGE IP 10.8.2.5 MASK /29"
RESP=$(send_command "GET HOSTS RANGE IP 10.8.2.5 MASK /29")
echo "   Respuesta: $RESP"

echo "4. GET NETMASK MASK /27"
RESP=$(send_command "GET NETMASK MASK /27")
echo "   Respuesta: $RESP"

echo "5. GET HOSTS COUNT MASK /29"
RESP=$(send_command "GET HOSTS COUNT MASK /29")
echo "   Respuesta: $RESP"

echo "6. GET IP CLASS 10.8.2.5"
RESP=$(send_command "GET IP CLASS 10.8.2.5")
echo "   Respuesta: $RESP"

echo "7. GET RANDOM SUBNETS NETWORK NUMBER 10.0.0.0 MASK /8 NUMBER 3 SIZE /24"
RESP=$(send_command "GET RANDOM SUBNETS NETWORK NUMBER 10.0.0.0 MASK /8 NUMBER 3 SIZE /24")
echo "   Respuesta: $RESP"

echo "8. SET ROUTE LAN NETWORK NUMBER 192.168.1.0 MASK /24 PRIORITY 1"
RESP=$(send_command "SET ROUTE LAN NETWORK NUMBER 192.168.1.0 MASK /24 PRIORITY 1")
echo "   Respuesta: $RESP"

echo "9. ROUTE IP 192.168.1.50 DEBUG false"
RESP=$(send_command "ROUTE IP 192.168.1.50 DEBUG false")
echo "   Respuesta: $RESP"

echo "10. DEL ROUTE LAN"
RESP=$(send_command "DEL ROUTE LAN")
echo "   Respuesta: $RESP"

echo -e "\nTodas las pruebas de comunicación completadas con éxito."
