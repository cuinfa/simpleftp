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

// Función para enviar mensajes al servidor
void send_msg(int sd, const char *cmd, const char *param) {
    char buffer[BUFSIZE];
    if (param)
        snprintf(buffer, sizeof(buffer), "%s %s\r\n", cmd, param);
    else
        snprintf(buffer, sizeof(buffer), "%s\r\n", cmd);
    send(sd, buffer, strlen(buffer), 0);
}

// Función para recibir mensajes del servidor
bool recv_msg(int sd, int expect_code, char *buffer) {
    char recv_buffer[BUFSIZE];
    int code;

    int recv_len = recv(sd, recv_buffer, sizeof(recv_buffer) - 1, 0);
    if (recv_len < 0) {
        perror("Error receiving data");
        return false;
    }
    recv_buffer[recv_len] = '\0';

    sscanf(recv_buffer, "%d", &code);
    if (code != expect_code) {
        if (buffer) strcpy(buffer, recv_buffer);
        return false;
    }
    if (buffer) strcpy(buffer, recv_buffer);
    return true;
}

// Función para leer la entrada del usuario
char *read_input() {
    char *input = malloc(BUFSIZE);
    if (fgets(input, BUFSIZE, stdin)) {
        input[strcspn(input, "\n")] = 0; // Eliminar el carácter de nueva línea
        return input;
    }
    free(input);
    return NULL;
}

// Función para autenticar al usuario
void authenticate(int sd) {
    char *input;

    printf("username: ");
    input = read_input();
    send_msg(sd, "USER", input);
    free(input);

    if (recv_msg(sd, 331, NULL)) {
        printf("passwd: ");
        input = read_input();
        send_msg(sd, "PASS", input);
        free(input);

        if (!recv_msg(sd, 230, NULL)) {
            recv_msg(sd, 530, NULL);
            exit(1);
        }
    } else {
        perror("Message of password doesn't print");
        exit(1);
    }
}

// Función para enviar el comando PORT al servidor
void port(int sd, const char *ip, int port) {
    char buffer[BUFSIZE];
    snprintf(buffer, sizeof(buffer), "%s,%d,%d",
             ip, port / 256, port % 256);
    send_msg(sd, "PORT", buffer);
    if (!recv_msg(sd, 200, NULL)) {
        perror("Error in PORT command");
    }
}

// Función para descargar un archivo del servidor
void get(int sd, char *file_name) {
    char buffer[BUFSIZE];
    int file_size, recv_s;
    FILE *file;
    int data_sd;
    struct sockaddr_in data_addr;
    socklen_t addr_len = sizeof(data_addr);

    // Configuración de la conexión de datos
    data_sd = socket(PF_INET, SOCK_STREAM, 0);
    data_addr.sin_family = AF_INET;
    data_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    data_addr.sin_port = 0; // Dejar que el SO asigne un puerto

    if (bind(data_sd, (struct sockaddr *) &data_addr, sizeof(data_addr)) < 0) {
        perror("Error binding data socket");
        close(data_sd);
        return;
    }

    if (listen(data_sd, 1) < 0) {
        perror("Error listening on data socket");
        close(data_sd);
        return;
    }

    getsockname(data_sd, (struct sockaddr *) &data_addr, &addr_len);
    char ip[16];
    inet_ntop(AF_INET, &data_addr.sin_addr, ip, sizeof(ip));
    int port = ntohs(data_addr.sin_port);

    // Enviar comando PORT al servidor
    port(sd, ip, port);

    // Enviar comando RETR
    send_msg(sd, "RETR", file_name);

    if (recv_msg(sd, 299, buffer)) {
        sscanf(buffer, "File %*s size %d bytes", &file_size);

        file = fopen(file_name, "wb");
        if (file == NULL) {
            perror("Error opening file");
            return;
        }

        int client_sd = accept(data_sd, (struct sockaddr *) &data_addr, &addr_len);
        if (client_sd < 0) {
            perror("Error accepting data connection");
            fclose(file);
            close(data_sd);
            return;
        }

        while (file_size > 0) {
            recv_s = recv(client_sd, buffer, BUFSIZE, 0);
            if (recv_s <= 0) {
                perror("Error receiving file data");
                break;
            }

            fwrite(buffer, 1, recv_s, file);
            file_size -= recv_s;
        }

        close(client_sd);
        fclose(file);

        if (recv_msg(sd, 226, NULL)) {
            printf("File transfer complete\n");
        } else {
            printf("Error during file transfer\n");
        }
    } else {
        recv_msg(sd, 550, NULL);
    }

    close(data_sd);
}

// Función para enviar el comando QUIT al servidor
void quit(int sd) {
    send_msg(sd, "QUIT", NULL);
    recv_msg(sd, 221, NULL);
}

// Función para manejar las operaciones del cliente
void operate(int sd) {
    char *input, *op, *param;

    while (true) {
        printf("Operation: ");
        input = read_input();
        if (input == NULL)
            continue;
        op = strtok(input, " ");

        if (strcmp(op, "get") == 0) {
            param = strtok(NULL, " ");
            if (param) get(sd, param);
            else printf("Filename required\n");
        } else if (strcmp(op, "quit") == 0) {
            quit(sd);
            break;
        } else {
            printf("TODO: unexpected command\n");
        }
        free(input);
    }
    free(input);
}

int main(int argc, char *argv[]) {
    int sd;
    struct sockaddr_in addr;

    if (argc != 3) {
        perror("Input must contain only three elements");
        exit(EXIT_FAILURE);
    }

    sd = socket(PF_INET, SOCK_STREAM, 0);
    if (sd == -1) {
        perror("Something went wrong setting the socket");
        exit(EXIT_FAILURE);
    }

    addr.sin_family = AF_INET;
    memset(addr.sin_zero, '\0', sizeof(addr.sin_zero));
    addr.sin_port = htons(atoi(argv[2]));

    if (inet_pton(AF_INET, argv[1], &(addr.sin_addr)) <= 0) {
        perror("Invalid address");
        exit(EXIT_FAILURE);
    }

    if (connect(sd, (struct sockaddr *) &addr, sizeof(addr)) == -1) {
        perror("Can't establish connection with server");
        close(sd);
        exit(EXIT_FAILURE);
    }

    if (recv_msg(sd, 220, NULL)) {
        authenticate(sd);
        operate(sd);
    } else {
        perror("Something went wrong with HELLO message");
    }

    close(sd);
    return 0;
}
