#include <arpa/inet.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdarg.h>
#include <sys/socket.h>
#include <unistd.h>
#include <err.h>
#include <netinet/in.h>

#define BUFSIZE 512
#define CMDSIZE 4
#define PARSIZE 100

#define MSG_220 "220 srvFtp version 1.0\r\n"
#define MSG_331 "331 Password required for %s\r\n"
#define MSG_230 "230 User %s logged in\r\n"
#define MSG_530 "530 Login incorrect\r\n"
#define MSG_221 "221 Goodbye\r\n"
#define MSG_550 "550 %s: no such file or directory\r\n"
#define MSG_299 "299 File %s size %ld bytes\r\n"
#define MSG_226 "226 Transfer complete\r\n"
#define MSG_200 "200 Command okay\r\n"

typedef struct {
    int data_sd;
    struct sockaddr_in data_addr;
} data_connection_t;

// Función para recibir comandos del cliente
bool recv_cmd(int sd, char *operation, char *param) {
    char buffer[BUFSIZE], *token;
    int recv_s;

    recv_s = recv(sd, buffer, BUFSIZE, 0);
    if (recv_s < 0) warn("error receiving data");
    if (recv_s == 0) errx(1, "connection closed by host");

    buffer[strcspn(buffer, "\r\n")] = 0;

    token = strtok(buffer, " ");
    if (token == NULL || strlen(token) < 4) {
        warn("not valid ftp command");
        return false;
    } else {
        if (operation[0] == '\0') strcpy(operation, token);
        token = strtok(NULL, " ");
        if (token != NULL) strcpy(param, token);
    }
    return true;
}

// Función para enviar respuestas al cliente
bool send_ans(int sd, const char *message, ...) {
    va_list args;
    char buffer[BUFSIZE];

    va_start(args, message);
    vsprintf(buffer, message, args);
    va_end(args);

    if (send(sd, buffer, strlen(buffer), 0) == -1) {
        warn("error sending data to client");
        return false;
    }
    return true;
}

// Función para autenticar al usuario
bool authenticate(int sd) {
    char user[PARSIZE], pass[PARSIZE], operation[CMDSIZE];

    if (recv_cmd(sd, "USER", user)) {
        if (send_ans(sd, MSG_331, user)) {
            if (recv_cmd(sd, "PASS", pass)) {
                if (!strcmp(user, "admin") && !strcmp(pass, "admin")) {
                    return send_ans(sd, MSG_230, user);
                } else {
                    return send_ans(sd, MSG_530);
                }
            }
        }
    }
    return false;
}

// Función para manejar el comando PORT
data_connection_t handle_port_command(int sd, char *param) {
    data_connection_t data_conn;
    int h1, h2, h3, h4, p1, p2;
    char ip[16];

    sscanf(param, "%d,%d,%d,%d,%d,%d", &h1, &h2, &h3, &h4, &p1, &p2);
    snprintf(ip, sizeof(ip), "%d.%d.%d.%d", h1, h2, h3, h4);

    data_conn.data_sd = socket(PF_INET, SOCK_STREAM, 0);
    if (data_conn.data_sd < 0) {
        perror("Error creating data socket");
        data_conn.data_sd = -1;
        return data_conn;
    }

    data_conn.data_addr.sin_family = AF_INET;
    data_conn.data_addr.sin_port = htons(p1 * 256 + p2);
    inet_pton(AF_INET, ip, &data_conn.data_addr.sin_addr);

    if (connect(data_conn.data_sd, (struct sockaddr *) &data_conn.data_addr, sizeof(data_conn.data_addr)) < 0) {
        perror("Error connecting data socket");
        close(data_conn.data_sd);
        data_conn.data_sd = -1;
    }

    return data_conn;
}

// Función para transferir un archivo al cliente
bool retr(int sd, char *file_name, data_connection_t data_conn) {
    FILE *file;
    char buffer[BUFSIZE];
    long file_size;
    int read_s;

    file = fopen(file_name, "rb");
    if (file == NULL) {
        send_ans(sd, MSG_550, file_name);
        return false;
    }

    fseek(file, 0L, SEEK_END);
    file_size = ftell(file);
    rewind(file);

    if (!send_ans(sd, MSG_299, file_name, file_size)) {
        fclose(file);
        return false;
    }

    while ((read_s = fread(buffer, 1, BUFSIZE, file)) > 0) {
        if (send(data_conn.data_sd, buffer, read_s, 0) < 0) {
            perror("Error sending file data");
            fclose(file);
            return false;
        }
    }

    fclose(file);
    close(data_conn.data_sd);
    return send_ans(sd, MSG_226);
}

// Función para manejar las operaciones del cliente
void operate(int sd) {
    char operation[CMDSIZE], param[PARSIZE];
    data_connection_t data_conn;

    while (true) {
        operation[0] = '\0';
        param[0] = '\0';

        if (recv_cmd(sd, operation, param)) {
            if (!strcmp(operation, "QUIT")) {
                send_ans(sd, MSG_221);
                break;
            } else if (!strcmp(operation, "RETR")) {
                if (data_conn.data_sd != -1)
                    retr(sd, param, data_conn);
                else
                    send_ans(sd, MSG_550, param);
            } else if (!strcmp(operation, "PORT")) {
                data_conn = handle_port_command(sd, param);
                if (data_conn.data_sd != -1) send_ans(sd, MSG_200);
            } else {
                warn("command not implemented: %s", operation);
            }
        }
    }
}

int main(int argc, char *argv[]) {
    int master_sd, slave_sd;
    struct sockaddr_in master_addr, slave_addr;

    if (argc != 2) errx(EXIT_FAILURE, "usage: %s port", argv[0]);

    master_sd = socket(PF_INET, SOCK_STREAM, 0);
    if (master_sd == -1) err(EXIT_FAILURE, "cannot create socket");

    master_addr.sin_family = AF_INET;
    master_addr.sin_port = htons(atoi(argv[1]));
    master_addr.sin_addr.s_addr = htonl(INADDR_ANY);

    if (bind(master_sd, (struct sockaddr *) &master_addr, sizeof(master_addr)) == -1) {
        perror("Cannot bind socket");
        close(master_sd);
        exit(EXIT_FAILURE);
    }

    if (listen(master_sd, 3) == -1) {
        perror("Cannot listen on socket");
        close(master_sd);
        exit(EXIT_FAILURE);
    }

    socklen_t addrlen = sizeof(slave_addr);
    while ((slave_sd = accept(master_sd, (struct sockaddr *) &slave_addr, &addrlen)) > 0) {
        if (send_ans(slave_sd, MSG_220)) {
            if (authenticate(slave_sd)) {
                operate(slave_sd);
            }
        }
        close(slave_sd);
    }

    close(master_sd);
    return 0;
}
