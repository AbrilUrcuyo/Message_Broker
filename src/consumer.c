#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>

#define SERVER_IP "127.0.0.1"
#define SERVER_PORT 8080
#define MAX_MESSAGE_LENGTH 256

// Definición del mensaje
typedef struct {
    int id;
    char mensaje[MAX_MESSAGE_LENGTH];
} Mensaje;

// Prototipos
int crear_socket();
void conectar_al_broker(int sock, struct sockaddr_in *broker_addr);
void solicitar_mensajes(int sock);
void recibir_mensajes(int sock);

// Función principal
int main() {
    int sock;
    struct sockaddr_in broker_addr;

    sock = crear_socket();
    conectar_al_broker(sock, &broker_addr);
    solicitar_mensajes(sock);
    recibir_mensajes(sock);

    close(sock);
    return 0;
}

// Crear y configurar socket
int crear_socket() {
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        perror("Error al crear el socket");
        exit(EXIT_FAILURE);
    }
    return sock;
}

// Conectar al broker
void conectar_al_broker(int sock, struct sockaddr_in *broker_addr) {
    broker_addr->sin_family = AF_INET;
    broker_addr->sin_port = htons(SERVER_PORT);
    if (inet_pton(AF_INET, SERVER_IP, &broker_addr->sin_addr) <= 0) {
        perror("Dirección IP inválida");
        exit(EXIT_FAILURE);
    }

    if (connect(sock, (struct sockaddr *)broker_addr, sizeof(*broker_addr)) < 0) {
        perror("Error de conexión con el broker");
        exit(EXIT_FAILURE);
    }

    printf("Conectado al broker en %s:%d\n", SERVER_IP, SERVER_PORT);
}

// Enviar solicitud GET
void solicitar_mensajes(int sock) {
    char *peticion = "GET";
    if (send(sock, peticion, strlen(peticion), 0) < 0) {
        perror("Error al enviar solicitud");
        exit(EXIT_FAILURE);
    }
}

// Recibir objetos tipo Mensaje
void recibir_mensajes(int sock) {
    Mensaje mensaje;

    printf("Esperando mensajes...\n");
    while (1) {
        memset(&mensaje, 0, sizeof(Mensaje));
        int bytes = recv(sock, &mensaje, sizeof(Mensaje), 0);
        if (bytes <= 0) {
            printf("Conexión cerrada por el broker.\n");
            break;
        }

        printf("Mensaje recibido:\n");
        printf("  ID: %d\n", mensaje.id);
        printf("  Contenido: %s\n", mensaje.mensaje);
    
    }
}
