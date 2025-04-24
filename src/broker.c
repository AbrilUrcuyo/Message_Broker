//
//          EN ESTA CLASE UN 1 MEMORY LEAK y NO HAY RACE CONDITIONS
//


#include <stdio.h>
#include <stdlib.h>  // Para malloc, free, strdup
#include <string.h> 
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <semaphore.h>



#define MAX_MESSAGE_LENGTH 256
#define PORT 8080
#define MAX_CONNECTIONS 10
#define QUEUE_CAPACITY 5


sem_t espacios_disponibles;
sem_t mensajes_disponibles;
pthread_mutex_t mutexCola = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t persister_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t log_mutex= PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t mutexKeepRunning = PTHREAD_MUTEX_INITIALIZER;


typedef struct {
    int id;
    char mensaje[MAX_MESSAGE_LENGTH];
} Mensaje;


//       IMPLEMENTACION DE LA COLA CIRCULAR
typedef struct {
    Mensaje mensajes[QUEUE_CAPACITY];
    int front;
    int rear;
    int size;
} ColaCircular;

void initQueue(ColaCircular* queue) {
    queue->front = 0;
    queue->rear = 0;
    queue->size = 0;
}

int isFull(ColaCircular* queue) {
    return queue->size == QUEUE_CAPACITY;
}

int isEmpty(ColaCircular* queue) {
    return queue->size == 0;
}

//Revisar en los capítulos, creo que el manejo de los mutex lo manejaban de manera más eficiente, pero eso puede quedar para el final
int enqueue(ColaCircular* queue, Mensaje mensaje) {
    sem_wait(&espacios_disponibles);   
    pthread_mutex_lock(&mutexCola);  // Bloquear el mutex al comenzar

    if (queue->size == QUEUE_CAPACITY) {
        pthread_mutex_unlock(&mutexCola);  // Desbloquear antes de salir
        return -1; // Cola llena
    }

    queue->mensajes[queue->rear] = mensaje;
    queue->rear = (queue->rear + 1) % QUEUE_CAPACITY;
    queue->size++;

    pthread_mutex_unlock(&mutexCola);  // Desbloquear al finalizar
    sem_post(&mensajes_disponibles);  
    return 0;
}


int dequeue(ColaCircular* queue, Mensaje* mensaje) {
    sem_wait(&mensajes_disponibles);  
    pthread_mutex_lock(&mutexCola);  // Bloquear el mutex al comenzar

    if (queue->size == 0) {
        pthread_mutex_unlock(&mutexCola);  // Desbloquear antes de salir
        return -1; // Cola vacía
    }

    *mensaje = queue->mensajes[queue->front];
    queue->front = (queue->front + 1) % QUEUE_CAPACITY;
    queue->size--;

    pthread_mutex_unlock(&mutexCola);  // Desbloquear al finalizar
    sem_post(&espacios_disponibles);   
    return 0;
}



//                    ARCHIVOS .LOG Y PERSISTER 
FILE* abrir_archivo(const char* nombre_archivo) {
    FILE* archivo = fopen(nombre_archivo, "a");  // Modo "a" para agregar al archivo sin sobrescribir
    if (archivo == NULL) {
        perror("No se pudo abrir el archivo");
        exit(1);  // Termina el programa si no puede abrir el archivo
    }
    return archivo;
}
//               ESCRIBIR EN LOG
void escribir_log(FILE* archivo, const char* mensaje) {
    pthread_mutex_lock(&log_mutex);
    fprintf(archivo, "%s\n", mensaje);
    fflush(archivo); // Aseguramos que se escriba inmediatamente
    pthread_mutex_unlock(&log_mutex);
}
//             ESCRIBIR EN PERSISTER

void escribir_persister(FILE* archivo, Mensaje *mensaje) {
    pthread_mutex_lock(&persister_mutex);
    fprintf(archivo, "[%d]-", mensaje->id); // ID entre corchetes
    fprintf(archivo, "%s\n", mensaje->mensaje);
    fflush(archivo); // Aseguramos que se escriba inmediatamente
    pthread_mutex_unlock(&persister_mutex);
}
void cerrar_archivo(FILE* archivo) {
    fclose(archivo);
}
//-----------------------------------------------------------------------------------

// Variables Globales
ColaCircular colaGlobal; //No se si deberian ser locales
FILE* archivoLog;        //No se si deberian ser locales
FILE* persisterFile;

int server_fd;
volatile int keepRunning = 1;


//      ESTO MUTEX ES PARA MANEJAR QUE EL INCREMENTO SE HAGA ADECUDAMENTE
pthread_mutex_t mutexID = PTHREAD_MUTEX_INITIALIZER;
int contadorMensajes=0;

//         ASIGNACION DE ID AL MSJ (NO DEPENDE DEL ID DEL PRODUCER)
int obtener_id_mensaje() {
    pthread_mutex_lock(&mutexID);
    int id = contadorMensajes++;
    pthread_mutex_unlock(&mutexID);
    return id;
}

int get_keep_running() {
    pthread_mutex_lock(&mutexKeepRunning);
    int value = keepRunning;
    pthread_mutex_unlock(&mutexKeepRunning);
    return value;
}

void set_keep_running(int value) {
    pthread_mutex_lock(&mutexKeepRunning);
    keepRunning = value;
    pthread_mutex_unlock(&mutexKeepRunning);
}

// Maneja la se�al para terminar el programa
void handle_signal(int sig) {
    printf("\nRecibida se�al de terminaci�n. Cerrando broker...\n");
    set_keep_running(0);
    close(server_fd);
}

// Funci�n para procesar mensajes de la cola y guardarlos en el archivo
// void* procesador_mensajes(void* arg) {
//     while (keepRunning) {
//         Mensaje mensaje;
//         if (dequeue(&colaGlobal, &mensaje) == 0) {
//             escribir_log(archivoLog, mensaje.mensaje);
//             printf("Guardado en log: %s\n", mensaje.mensaje);
//         }
//         usleep(100000); // Esperar 100ms
//     }
//     return NULL;
// }

//                           SOCKETS 
// Maneja la conexi�n con un cliente
//Es mejor separarlo, hacer dos funciones, una para el consumer y otra para el producer y llamarlas aquí. 
//Para el consumer se puede aprovechar el procesador de mensajes.
void* handle_client(void* socket_desc) {
    int client_sock = *(int*)socket_desc;
    free(socket_desc);

    char tipo[4] = {0};  // Buffer para verificar si es "GET"

    int read_size = recv(client_sock, tipo, 3, MSG_PEEK);  // Leer sin consumir
    if (read_size <= 0) {
        close(client_sock);
        return NULL;
    }

    if (strncmp(tipo, "GET", 3) == 0) {
        // Es un consumer
        char dummy[4];
        recv(client_sock, dummy, 3, 0);  // Consumimos los 3 bytes del "GET"
        printf("Consumer conectado\n");

        while (get_keep_running()) {
            Mensaje mensaje;
            if (dequeue(&colaGlobal, &mensaje) == 0) { //Aquí saca el mensaje de la cola, igual que cuando lo quiere meter en el archivo, hay que juntarlo para que se le hagan las dos operaciones al mismo mensaje.
                send(client_sock, &mensaje, sizeof(Mensaje), 0);
                printf("Mensaje enviado al consumer: %s\n", mensaje.mensaje);
                escribir_log(archivoLog, mensaje.mensaje);
                printf("Guardado en log: %s\n", mensaje.mensaje);
            } else {
                usleep(100000);  // Espera un poco si la cola está vacía
            }
        }

    } else {
        // Es un producer
        Mensaje nuevoMensaje;
        while ((read_size = recv(client_sock, &nuevoMensaje, sizeof(Mensaje), 0)) > 0) {
            nuevoMensaje.id=obtener_id_mensaje();
            printf("Mensaje recibido: %s\n", nuevoMensaje.mensaje);
            escribir_persister(persisterFile, &nuevoMensaje);

            if (enqueue(&colaGlobal, nuevoMensaje) == -1) {
                printf("Cola llena, mensaje descartado\n");
            }
        }

        if (read_size == 0) {
            printf("Producer desconectado\n");
        } else if (read_size == -1) {
            perror("Error en recv");
        }
    }

    close(client_sock);
    return NULL;
}




int main() {
    struct sockaddr_in address;
    int addrlen = sizeof(address);
    pthread_t processor_thread;
    sem_init(&espacios_disponibles, 0, QUEUE_CAPACITY); // espacios disponibles al inicio
    sem_init(&mensajes_disponibles, 0, 0);

    // Inicializar la cola
    initQueue(&colaGlobal);

    // Abrir el archivo de log
    archivoLog = abrir_archivo("archivo.log");
    persisterFile = abrir_archivo("persistencia.txt");

    // Configurar el manejo de se�ales
    signal(SIGINT, handle_signal);

    // Crear socket
    if ((server_fd = socket(AF_INET, SOCK_STREAM, 0)) == 0) {
        perror("Fallo en la creaci�n del socket");
        exit(EXIT_FAILURE);
    }

    // Para permitir reutilizaci�n del puerto
    int opt = 1;
    if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt))) {
        perror("setsockopt");
        exit(EXIT_FAILURE);
    }

    // Configurar direcci�n del socket
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(PORT);

    // Enlazar el socket al puerto
    if (bind(server_fd, (struct sockaddr*)&address, sizeof(address)) < 0) {
        perror("Fallo en bind");
        exit(EXIT_FAILURE);
    }

    // Escuchar por conexiones entrantes
    if (listen(server_fd, MAX_CONNECTIONS) < 0) { //Tal vez sería bueno cambiar este máximo, es de la cantidad de conexiones que pueden estar en espera.
        perror("Fallo en listen");
        exit(EXIT_FAILURE);
    }

    printf("Broker iniciado en puerto %d\n", PORT);

    // Aceptar conexiones entrantes
    while (get_keep_running()) {
        int new_socket;
        if ((new_socket = accept(server_fd, (struct sockaddr*)&address, (socklen_t*)&addrlen)) < 0) {
            if (!get_keep_running()) break; // Si se cerr� por la se�al, es normal
            perror("Fallo en accept");
            continue;
        }

        printf("Nueva conexi�n aceptada\n");

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
            // Desvincular el hilo para que se limpie autom�ticamente
            pthread_detach(client_thread);
        }
    }

    cerrar_archivo(persisterFile);
    cerrar_archivo(archivoLog);
    sem_destroy(&espacios_disponibles);
    sem_destroy(&mensajes_disponibles);
    close(server_fd);

    printf("Broker finalizado\n");
    return 0;
}