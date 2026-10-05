#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <ctype.h>

#define MAX_LINE   512
#define MAX_TOKENS 16



//Pasa la IP a decimal
static void ip_to_str(uint32_t ip, char *buf, size_t buflen) {
    snprintf(buf, buflen, "%u.%u.%u.%u",
             (ip >> 24) & 0xFFu,
             (ip >> 16) & 0xFFu,
             (ip >>  8) & 0xFFu,
              ip        & 0xFFu);
}

// Se fija que la IP sea valida, y si es asi, se va a decimal
static int parse_ip(const char *str, uint32_t *out) {
    unsigned int a, b, c, d;
    if (!str || !*str) return 0;
    if (sscanf(str, "%u.%u.%u.%u", &a, &b, &c, &d) != 4) return 0;
    if (a > 255u || b > 255u || c > 255u || d > 255u) return 0;
    //Revisa que ninguno se pase de 255
    *out = ((uint32_t)a << 24) | ((uint32_t)b << 16) |
           ((uint32_t)c <<  8) |  (uint32_t)d;
    return 1;
}

//Recibe la mascara
static int parse_mask(const char *str, uint32_t *mask_out) {
    //Hay algo o no?
    if (!str || !*str) return -1;
    const char *p = str;
    if (*p == '/') p++;
    //Si es e; / se lo salta para que no lo tome en cuenta

    //Revisa si hay solo numeros o si hay puntos
    int is_number = (*p != '\0');
    for (const char *q = p; *q; q++) {
        if (!isdigit((unsigned char)*q)) { is_number = 0; break; }
    }
    //Esto es sopo para cuando es numero
    if (is_number) {
        char *endp = NULL;
        long v = strtol(p, &endp, 10);
        if (!endp || *endp != '\0') return -1;
        if (v < 0 || v > 32) return -1;// error si > 32 
        if (mask_out) {
            if (v == 0) *mask_out = 0u;
            else        *mask_out = 0xFFFFFFFFu << (32 - (int)v);
            //Lo convierte a bits 
        }
        return (int)v;
    }

    //Esto es cuando es una ip 
    uint32_t m;
    if (!parse_ip(str, &m)) return -1; //Este convierte la ip a decimal

    //Cuenta cuantos 1 hay y se detiene cuando encuentre 0
    int prefix = 0;
    int found_zero = 0;
    for (int i = 31; i >= 0; i--) {
        if (m & (1u << i)) {
            if (found_zero) return -1; //Aca es cuando encuentra un 1 despues de un 0             
            prefix++;
        } else {
            found_zero = 1;
        }
    }
    if (mask_out) *mask_out = m;
    return prefix;
}

static int prefix_from_mask(uint32_t mask) {
    int p = 0;
    for (int i = 31; i >= 0; i--) {
        if (mask & (1u << i)) p++;
        else break;
    }
    return p;
    //Recorre desde izquerda a derecha, contando los 1 para ver cuantos hay, se detiene al 0
}



static uint32_t get_network  (uint32_t ip, uint32_t mask) { return ip & mask; }




static void cmd_broadcast(uint32_t ip, uint32_t mask) {
    char bcb[32];
    uint32_t net = get_network(ip, mask);
    uint32_t bc  = net | ~mask;
    ip_to_str(bc,      bcb,  sizeof bcb);

    printf("Broadcast: %s\n", bcb);
}

static void cmd_network(uint32_t ip, uint32_t mask) {
    char netb[32];
    ip_to_str(get_network(ip, mask), netb, sizeof netb);
    printf("Network: %s\n", netb);
}

static void cmd_hosts_range(uint32_t ip, uint32_t mask) {
    uint32_t net = get_network(ip, mask);
    uint32_t bc  = net | ~mask;
    uint32_t first, last;
    int p = prefix_from_mask(mask);

    if (p == 32) {
        first = last = net;          /* host único */
    } else if (p == 31) {
        first = net;                 /* RFC 3021: ambos son hosts */
        last  = bc;
    } else {
        first = net + 1;
        last  = bc  - 1;
    }

    char nb[32], bb[32], b1[32], b2[32];
    ip_to_str(net,   nb, sizeof nb);
    ip_to_str(bc,    bb, sizeof bb);
    ip_to_str(first, b1, sizeof b1);
    ip_to_str(last,  b2, sizeof b2);


    printf("Hosts range: %s - %s\n", b1, b2);
}

static void cmd_netmask(uint32_t mask, int prefix) {
    char buf[32];
    ip_to_str(mask, buf, sizeof buf);
    printf("Netmask: %s (/%d)\n", buf, prefix);
}

static void cmd_hosts_count(int prefix) {
    if (prefix == 32) {
        printf("Hosts count: 1 (host único)\n");
        return;
    }
    if (prefix == 31) {
        printf("Hosts count: 2 (enlace punto a punto, RFC 3021)\n");
        return;
    }
    unsigned long long block = 1ULL << (32 - prefix);
    printf("Hosts count: %llu\n", block - 2ULL);
}

static char ip_class(uint32_t ip, int *default_prefix) {
    uint8_t first = (ip >> 24) & 0xFFu;   //Solo revisa el primer byte

    if (first <= 127)      { if (default_prefix) *default_prefix = 8;  return 'A'; }
    if (first <= 191)      { if (default_prefix) *default_prefix = 16; return 'B'; }
    if (first <= 223)      { if (default_prefix) *default_prefix = 24; return 'C'; }
    if (first <= 239)      { if (default_prefix) *default_prefix = -1; return 'D'; }
    if (default_prefix) *default_prefix = -1;
    return 'E';
}



static void to_upper(char *s) {
    for (; *s; s++) *s = (char)toupper((unsigned char)*s);
}

static int ieq(const char *a, const char *b) {
    while (*a && *b) {
        if (toupper((unsigned char)*a) != toupper((unsigned char)*b)) return 0;
        a++; b++;
    }
    return *a == '\0' && *b == '\0';
}





static void process_line(const char *input) {
    char buf[MAX_LINE];
    strncpy(buf, input, sizeof buf - 1);
    buf[sizeof buf - 1] = '\0';

    //separar usando espacios
    char *tokens[MAX_TOKENS];
    int ntok = 0;
    char *tok = strtok(buf, " \t");
    while (tok && ntok < MAX_TOKENS) {
        tokens[ntok++] = tok;
        tok = strtok(NULL, " \t");
    }
    if (ntok == 0) return;

    char cmd[32];
    strncpy(cmd, tokens[0], sizeof cmd - 1);
    cmd[sizeof cmd - 1] = '\0';
    to_upper(cmd);

    /* --- NETMASK <mask> --- */
    if (strcmp(cmd, "NETMASK") == 0) {
        if (ntok < 2) { printf("Uso: NETMASK <mask>\n"); return; }
        uint32_t mask;
        int prefix = parse_mask(tokens[1], &mask);
        if (prefix < 0) {
            printf("Error: máscara inválida (use /N o X.X.X.X, N entre 0 y 32)\n");
            return;
        }
        cmd_netmask(mask, prefix);
        return;
    }

    /* --- HOSTS RANGE / HOSTS COUNT --- */
    if (strcmp(cmd, "HOSTS") == 0) {
        if (ntok >= 2 && ieq(tokens[1], "COUNT")) {
            if (ntok < 4 || !ieq(tokens[2], "MASK")) {
                printf("Uso: HOSTS COUNT MASK <mask>\n"); return;
            }
            uint32_t mask;
            int prefix = parse_mask(tokens[3], &mask);
            if (prefix < 0) { printf("Error: máscara inválida\n"); return; }
            cmd_hosts_count(prefix);
            return;
        }
        if (ntok >= 2 && ieq(tokens[1], "RANGE")) {
            if (ntok < 6 || !ieq(tokens[2], "IP") || !ieq(tokens[4], "MASK")) {
                printf("Uso: HOSTS RANGE IP <ip> MASK <mask>\n"); return;
            }
            uint32_t ip, mask;
            if (!parse_ip(tokens[3], &ip)) { printf("Error: IP inválida\n"); return; }
            int prefix = parse_mask(tokens[5], &mask);
            if (prefix < 0) { printf("Error: máscara inválida\n"); return; }
            cmd_hosts_range(ip, mask);
            return;
        }
        printf("Uso: HOSTS RANGE IP <ip> MASK <mask> | HOSTS COUNT MASK <mask>\n");
        return;
    }

    /* --- BROADCAST IP <ip> MASK <mask> --- */
    if (strcmp(cmd, "BROADCAST") == 0) {
        if (ntok < 5 || !ieq(tokens[1], "IP") || !ieq(tokens[3], "MASK")) {
            printf("Uso: BROADCAST IP <ip> MASK <mask>\n"); return;
        }
        uint32_t ip, mask;
        if (!parse_ip(tokens[2], &ip)) { printf("Error: IP inválida\n"); return; }
        int prefix = parse_mask(tokens[4], &mask);
        if (prefix < 0) { printf("Error: máscara inválida\n"); return; }
        cmd_broadcast(ip, mask);
        return;
    }

    /* --- NETWORK IP <ip> MASK <mask> --- */
    if (strcmp(cmd, "NETWORK") == 0) {
        if (ntok < 5 || !ieq(tokens[2], "IP") || !ieq(tokens[4], "MASK")) {
            printf("Uso: NETWORK NUMBER IP <ip> MASK <mask>\n"); return;
        }
        uint32_t ip, mask;
        if (!parse_ip(tokens[3], &ip)) { printf("Error: IP inválida\n"); return; }
        int prefix = parse_mask(tokens[5], &mask);
        if (prefix < 0) { printf("Error: máscara inválida\n"); return; }
        cmd_network(ip, mask);
        return;
    }

    if (strcmp(cmd, "IP") == 0) {
    if (ntok >= 3 && ieq(tokens[1], "CLASS")) {
        uint32_t ip;
        if (!parse_ip(tokens[2], &ip)) {
            printf("Error: IP inválida\n");
            return;
        }
        int defp = -1;
        char cls = ip_class(ip, &defp);

        char ipb[32];
        ip_to_str(ip, ipb, sizeof ipb);

        printf("IP:          %s\n", ipb);
        printf("Class:       %c\n", cls);
        if (defp >= 0) {
            uint32_t defmask = 0xFFFFFFFFu << (32 - defp);
            char msk[32];
            ip_to_str(defmask, msk, sizeof msk);
            printf("Default mask: %s (/%d)\n", msk, defp);
        } else {
            printf("Default mask: — (clase %c no tiene máscara por defecto)\n", cls);
        }
        return;
    }
    printf("Uso: IP CLASS <ip>\n");
    return;
}


    printf("No se que escribio: %s \n Suerte con eso\n", tokens[0]);
}



int main(int argc, char **argv) {
    /* Modo directo: pasar comando por argumentos */
    if (argc > 1) {
        char line[MAX_LINE] = {0};
        size_t pos = 0;
        for (int i = 1; i < argc; i++) {
            size_t len = strlen(argv[i]);
            if (pos + len + 2 >= sizeof line) break;
            if (i > 1) line[pos++] = ' ';
            memcpy(line + pos, argv[i], len);
            pos += len;
        }
        process_line(line);
        return 0;
    }

    /* Modo interactivo */
    char line[MAX_LINE];
    printf("=== Calculadora de Subredes IPv4 ===\n");
    

    while (1) {
        printf("> ");
        fflush(stdout);
        if (!fgets(line, sizeof line, stdin)) break;
        line[strcspn(line, "\n")] = '\0';

        /* Detectar salida */
        char tmp[MAX_LINE];
        strncpy(tmp, line, sizeof tmp - 1);
        tmp[sizeof tmp - 1] = '\0';
        char *first = strtok(tmp, " \t");
        if (first) {
            char c[32];
            strncpy(c, first, sizeof c - 1);
            c[sizeof c - 1] = '\0';
            to_upper(c);
            if (strcmp(c, "EXIT") == 0 || strcmp(c, "QUIT") == 0) break;
            
        }
        process_line(line);
    }

    return 0;
}
