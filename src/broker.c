#include <stdio.h>
#include <stdlib.h>  // Para malloc, free, strdup
#include <string.h> 
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>

#define MAX_MESSAGE_LENGTH 256
#define PORT 8080
#define MAX_CONNECTIONS 10

// Creacion del nodo 
typedef struct Node {
    char message[MAX_MESSAGE_LENGTH];
    struct Node* next;
} Node;

// Creacion de la cola, con nodo next y el anterior 
typedef struct MessageQueue {
    Node* front;
    Node* rear;
    pthread_mutex_t mutex;  // Para proteger accesos concurrentes a la cola
} MessageQueue;

// Inicializar cola
void initQueue(MessageQueue* queue) {
    queue->front = NULL;
    queue->rear = NULL;
    pthread_mutex_init(&queue->mutex, NULL);
}

// Encola un mensaje
void enqueue(MessageQueue* queue, const char* message) {
    pthread_mutex_lock(&queue->mutex);

    Node* newNode = (Node*)malloc(sizeof(Node));
    strncpy(newNode->message, message, MAX_MESSAGE_LENGTH);
    newNode->next = NULL;

    if (queue->rear == NULL) {
        queue->front = queue->rear = newNode;
    }
    else {
        queue->rear->next = newNode;
        queue->rear = newNode;
    }

    pthread_mutex_unlock(&queue->mutex);
}

// Desencola un mensaje
char* dequeue(MessageQueue* queue) {
    pthread_mutex_lock(&queue->mutex);

    if (queue->front == NULL) {
        pthread_mutex_unlock(&queue->mutex);
        return NULL;
    }

    Node* temp = queue->front;
    char* message = strdup(temp->message); // devuelve una copia del string
    queue->front = queue->front->next;

    if (queue->front == NULL)
        queue->rear = NULL;

    free(temp);
    pthread_mutex_unlock(&queue->mutex);
    return message;
}

void freeQueue(MessageQueue* queue) {
    pthread_mutex_lock(&queue->mutex);

    while (queue->front != NULL) {
        Node* temp = queue->front;
        queue->front = queue->front->next;
        free(temp);
    }

    pthread_mutex_unlock(&queue->mutex);
    pthread_mutex_destroy(&queue->mutex);
}

// Apertura del archivo.log
FILE* abrir_archivo(const char* nombre_archivo) {
    FILE* archivo = fopen(nombre_archivo, "a");  // Modo "a" para agregar al archivo sin sobrescribir
    if (archivo == NULL) {
        perror("No se pudo abrir el archivo");
        exit(1);  // Termina el programa si no puede abrir el archivo
    }
    return archivo;
}

// Método para escribir un mensaje en el archivo
void escribir_log(FILE* archivo, const char* mensaje) {
    fprintf(archivo, "%s\n", mensaje);
    fflush(archivo); // Aseguramos que se escriba inmediatamente
}

// Método para cerrar el archivo
void cerrar_archivo(FILE* archivo) {
    fclose(archivo);
}

// Variables Globales
MessageQueue colaGlobal;
FILE* archivoLog;
int server_fd;
volatile int keepRunning = 1;

// Maneja la señal para terminar el programa
void handle_signal(int sig) {
    printf("\nRecibida señal de terminación. Cerrando broker...\n");
    keepRunning = 0;
    // Cerrar el socket para que accept() se libere
    close(server_fd);
}

// Función para procesar mensajes de la cola y guardarlos en el archivo
void* procesador_mensajes(void* arg) {
    while (keepRunning) {
        char* msg = dequeue(&colaGlobal);
        if (msg != NULL) {
            escribir_log(archivoLog, msg);
            printf("Guardado en log: %s\n", msg);
            free(msg);
        }
        usleep(100000); // Esperar 100ms si no hay mensajes
    }
    return NULL;
}

// Maneja la conexión con un cliente
void* handle_client(void* socket_desc) {
    int client_sock = *(int*)socket_desc;
    free(socket_desc);

    char buffer[MAX_MESSAGE_LENGTH];
    int read_size;

    while ((read_size = recv(client_sock, buffer, sizeof(buffer) - 1, 0)) > 0) {
        buffer[read_size] = '\0';
        printf("Mensaje recibido: %s\n", buffer);
        enqueue(&colaGlobal, buffer);
    }

    if (read_size == 0) {
        printf("Cliente desconectado\n");
    }
    else if (read_size == -1) {
        perror("Error en recv");
    }

    close(client_sock);
    return NULL;
}

int main() {
    struct sockaddr_in address;
    int addrlen = sizeof(address);
    pthread_t processor_thread;

    // Inicializar la cola
    initQueue(&colaGlobal);

    // Abrir el archivo de log
    archivoLog = abrir_archivo("archivo.log");

    // Configurar el manejo de señales
    signal(SIGINT, handle_signal);

    // Crear socket
    if ((server_fd = socket(AF_INET, SOCK_STREAM, 0)) == 0) {
        perror("Fallo en la creación del socket");
        exit(EXIT_FAILURE);
    }

    // Para permitir reutilización del puerto
    int opt = 1;
    if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt))) {
        perror("setsockopt");
        exit(EXIT_FAILURE);
    }

    // Configurar dirección del socket
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(PORT);

    // Enlazar el socket al puerto
    if (bind(server_fd, (struct sockaddr*)&address, sizeof(address)) < 0) {
        perror("Fallo en bind");
        exit(EXIT_FAILURE);
    }

    // Escuchar por conexiones entrantes
    if (listen(server_fd, MAX_CONNECTIONS) < 0) {
        perror("Fallo en listen");
        exit(EXIT_FAILURE);
    }

    printf("Broker iniciado en puerto %d\n", PORT);

    // Iniciar el hilo procesador de mensajes
    pthread_create(&processor_thread, NULL, procesador_mensajes, NULL);

    // Aceptar conexiones entrantes
    while (keepRunning) {
        int new_socket;
        if ((new_socket = accept(server_fd, (struct sockaddr*)&address, (socklen_t*)&addrlen)) < 0) {
            if (!keepRunning) break; // Si se cerró por la señal, es normal
            perror("Fallo en accept");
            continue;
        }

        printf("Nueva conexión aceptada\n");

        // Crear un hilo para manejar al cliente
        pthread_t client_thread;
        int* client_sock = malloc(sizeof(int));
        *client_sock = new_socket;

        if (pthread_create(&client_thread, NULL, handle_client, (void*)client_sock) < 0) {
            perror("No se pudo crear el hilo");
            close(new_socket);
            free(client_sock);
        }
        else {
            // Desvincular el hilo para que se limpie automáticamente
            pthread_detach(client_thread);
        }
    }

    // Esperar a que finalice el hilo procesador
    pthread_join(processor_thread, NULL);

    // Limpieza final
    freeQueue(&colaGlobal);
    cerrar_archivo(archivoLog);
    close(server_fd);

    printf("Broker finalizado\n");
    return 0;
}