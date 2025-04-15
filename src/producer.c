#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <arpa/inet.h>

#define NUM_HILOS 3
#define MENSAJES_POR_HILO 5
#define MAX_MESSAGE_LENGTH 256
#define SERVER_IP "127.0.0.1"  // Localhost/IP local
#define PORT 8080              // Mismo puerto que el broker

pthread_mutex_t mutex_socket = PTHREAD_MUTEX_INITIALIZER;
int sock = 0;  // Socket global para comunicación con el broker

// Inicializar conexión con el broker
int conectar_broker() {
    struct sockaddr_in serv_addr;

    // Crear socket
    if ((sock = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
        perror("Error en la creación del socket");
        return -1;
    }

    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons(PORT);

    // Convertir dirección IP de texto a binario
    if (inet_pton(AF_INET, SERVER_IP, &serv_addr.sin_addr) <= 0) {
        perror("Dirección inválida o no soportada");
        return -1;
    }

    // Conectar al broker
    if (connect(sock, (struct sockaddr*)&serv_addr, sizeof(serv_addr)) < 0) {
        perror("Error en la conexión");
        return -1;
    }

    printf("Conectado al broker en %s:%d\n", SERVER_IP, PORT);
    return 0;
}

// Función para enviar un mensaje al broker
int enviar_mensaje(const char* mensaje) {
    pthread_mutex_lock(&mutex_socket);
    int result = send(sock, mensaje, strlen(mensaje), 0);
    pthread_mutex_unlock(&mutex_socket);

    if (result < 0) {
        perror("Error al enviar mensaje");
        return -1;
    }

    return 0;
}

void* generar_mensajes(void* arg) {
    long id = (long)arg;

    for (int i = 0; i < MENSAJES_POR_HILO; i++) {
        // Crear mensaje
        char mensaje[MAX_MESSAGE_LENGTH];
        snprintf(mensaje, sizeof(mensaje), "Hilo %ld: mensaje %d", id, i + 1);

        // Enviar mensaje al broker
        if (enviar_mensaje(mensaje) == 0) {
            printf("Enviado: %s\n", mensaje);
        }

        // Esperar un poco entre mensajes
        usleep(200000); // 200ms
    }

    return NULL;
}

int main() {
    pthread_t hilos[NUM_HILOS];

    // Conectar con el broker
    if (conectar_broker() != 0) {
        fprintf(stderr, "No se pudo conectar al broker. Asegúrate de que esté en ejecución.\n");
        return 1;
    }

    // Crear hilos para generar mensajes
    for (long i = 0; i < NUM_HILOS; i++) {
        if (pthread_create(&hilos[i], NULL, generar_mensajes, (void*)i) != 0) {
            perror("Error al crear hilo");
            continue;
        }
    }

    // Esperar a que todos los hilos terminen
    for (int i = 0; i < NUM_HILOS; i++) {
        pthread_join(hilos[i], NULL);
    }

    printf("Todos los hilos han terminado. Cerrando conexión...\n");

    // Cerrar el socket
    close(sock);

    return 0;
}