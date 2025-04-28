//
//          EN ESTA CLASE UN 1 MEMORY LEAK --> Crear un hilo para manejar al cliente  y NO HAY RACE CONDITIONS
//          HAY POSIBLE MEMORY LEAK AL NO ESPERAR QUE TERMINEN LOS HILOS ANTES DE CERRAR EL BROKER -->451


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
#define QUEUE_CAPACITY 10
#define MAX_CONSUMERS_GRUPO 2


sem_t espacios_disponibles;
sem_t mensajes_disponibles;
pthread_mutex_t mutexCola = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t persister_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t log_mutex= PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t mutexKeepRunning = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t mutexID = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t consumer_mutex = PTHREAD_MUTEX_INITIALIZER;

//-------------------------CLASES O ESTRUCTURAS ------------------------------
typedef struct {
    int id;
    char mensaje[MAX_MESSAGE_LENGTH];
} Mensaje;

typedef struct {
    int id;
    int socket_fd;
} Consumer;

typedef struct {
    int id;
    Consumer consumidores[5];
    int count;               // Cantidad actual de consumidores
    int rr_index;            // Round robin
    pthread_mutex_t mutex;
} ConsumerGroup;

typedef struct GrupoNode {
    ConsumerGroup grupo;
    struct GrupoNode* siguiente;
} GrupoNode;

typedef struct {
    GrupoNode* cabeza;
    pthread_mutex_t mutex;
    int grupo_id_counter;
    int consumer_id_counter;//creo que no se usa
} ListaGrupos;

typedef struct NodoMensaje {
    Mensaje mensaje;
    struct NodoMensaje* siguiente;
} NodoMensaje;

typedef struct {
    NodoMensaje* cabeza;
    pthread_mutex_t mutex;
} ListaMensajes;

typedef struct {
    Mensaje mensajes[QUEUE_CAPACITY];
    int front;
    int rear;
    int size;
} ColaCircular;

//----------------------------------------------------------------------------
//------------------------DECLARACION DE VARIABLES----------------------------
ListaGrupos* lista;
ListaMensajes* listaMensajes;
ColaCircular colaGlobal; //No se si deberian ser locales


int contadorMensajes=0;
int consumers_activos = 0;

FILE* archivoLog;     
FILE* persisterFile;

int server_fd;
volatile int keepRunning = 1;

//----------------------------------------------------------------------------
//-------------------PRE-DECLARACION DE METODDOS------------------------------
void inicializar_lista();
void inicializar_lista_mensajes();

//       Mensajes
void agregar_mensaje(Mensaje mensaje);
Mensaje obtener_mensaje();
int obtener_id_mensaje();

//      Consumers
void consumer_conectado();
void consumer_desconectado();
int hay_consumers();
int  registrar_consumer(int socket_fd);
//     Cola Circular
void initQueue(ColaCircular* queue);
int isFull(ColaCircular* queue);
int isEmpty(ColaCircular* queue);
int enqueue(ColaCircular* queue, Mensaje mensaje);
int dequeue(ColaCircular* queue, Mensaje* mensaje);


void* distribuir_a_grupos(void *arg);
void* handle_client(void* socket_desc);
int get_keep_running();
void set_keep_running(int value);
void handle_signal(int sig);

//      Archivos
FILE* abrir_archivo(const char* nombre_archivo);
void escribir_persister(FILE* archivo, Mensaje *mensaje);
void escribir_log(FILE* archivo, const char* mensaje);
void cerrar_archivo(FILE* archivo);
void* escribir_lista_mensajes(void* arg);

//----------------------------------------------------------------------------



void inicializar_lista_mensajes() {
    listaMensajes = malloc(sizeof(ListaMensajes));
    if (listaMensajes == NULL) {
        perror("Fallo en malloc de lista");
        exit(EXIT_FAILURE);
    }

    listaMensajes->cabeza = NULL;
    pthread_mutex_init(&listaMensajes->mutex, NULL);
}
void agregar_mensaje(Mensaje mensaje) {
    NodoMensaje* nuevo = malloc(sizeof(NodoMensaje));
    if (nuevo == NULL) {
        perror("Fallo en malloc de nuevo mensaje");
        return;
    }

    pthread_mutex_lock(&listaMensajes->mutex);
    nuevo->mensaje = mensaje; // Copiar el mensaje
    nuevo->siguiente = listaMensajes->cabeza;
    listaMensajes->cabeza = nuevo;
    printf("Mensaje agregado: %s\n", mensaje.mensaje); // Para depuración
    pthread_mutex_unlock(&listaMensajes->mutex);
}
Mensaje obtener_mensaje() {
    Mensaje mensaje;
    pthread_mutex_lock(&listaMensajes->mutex);
    
    if (listaMensajes->cabeza == NULL) {
        pthread_mutex_unlock(&listaMensajes->mutex);
        mensaje.id = -1; // Indicar que no hay mensajes
        return mensaje; // Cola vacía
    }

    NodoMensaje* nodo = listaMensajes->cabeza;
    mensaje = nodo->mensaje; // Obtener el mensaje
    listaMensajes->cabeza = nodo->siguiente;
    free(nodo); // Liberar el nodo
    pthread_mutex_unlock(&listaMensajes->mutex);
    return mensaje;
}
void escribir_persister(FILE* archivo, Mensaje *mensaje) {
    pthread_mutex_lock(&persister_mutex);
    fprintf(archivo, "[%d]-", mensaje->id); // ID entre corchetes
    fprintf(archivo, "%s\n", mensaje->mensaje);
    fflush(archivo); // Aseguramos que se escriba inmediatamente
    pthread_mutex_unlock(&persister_mutex);
}
int obtener_id_mensaje() {
    pthread_mutex_lock(&mutexID);
    int id = contadorMensajes++;
    pthread_mutex_unlock(&mutexID);
    return id;
}
void consumer_conectado() {
    pthread_mutex_lock(&consumer_mutex);
    consumers_activos++;
    pthread_mutex_unlock(&consumer_mutex);
}
void consumer_desconectado() {
    pthread_mutex_lock(&consumer_mutex);
    consumers_activos--;
    pthread_mutex_unlock(&consumer_mutex);
}
int hay_consumers() {
    pthread_mutex_lock(&consumer_mutex);
    int r = consumers_activos;
    pthread_mutex_unlock(&consumer_mutex);
    return r;
}
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
    mensaje.id=obtener_id_mensaje(); //Asignar ID al mensaje
    escribir_persister(persisterFile, &mensaje); //Escribir en el archivo de persistencia

    queue->mensajes[queue->rear] = mensaje;
    queue->rear = (queue->rear + 1) % QUEUE_CAPACITY;
    queue->size++;

    //Mensaje log;
    //snprintf(log.mensaje, MAX_MESSAGE_LENGTH, "Mensaje recibido: %d     ", mensaje.id);
    //agregar_mensaje(log); // Agregar el mensaje a la lista de mensajes 
    
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
    fprintf(archivo, "%s", mensaje);
    fflush(archivo); // Aseguramos que se escriba inmediatamente
    pthread_mutex_unlock(&log_mutex);
}

void cerrar_archivo(FILE* archivo) {
    fclose(archivo);
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
// Maneja la se al para terminar el programa
void handle_signal(int sig) {
    printf("\nRecibida se al de terminaci n. Cerrando broker...\n");
    set_keep_running(0);
    close(server_fd);
}
int registrar_consumer(int socket_fd) {
    pthread_mutex_lock(&lista->mutex);

    GrupoNode* actual = lista->cabeza;
    while (actual) {
        pthread_mutex_lock(&actual->grupo.mutex);
        if (actual->grupo.count < MAX_CONSUMERS_GRUPO) {
            // Asignar el ID dentro del grupo basado en el número de consumidores
            int grupo_id = actual->grupo.id;
            actual->grupo.consumidores[actual->grupo.count].id = actual->grupo.count;  // ID dentro del grupo
            actual->grupo.consumidores[actual->grupo.count].socket_fd = socket_fd;
            actual->grupo.count++;
            pthread_mutex_unlock(&actual->grupo.mutex);
            pthread_mutex_unlock(&lista->mutex);
            return grupo_id;
        }
        pthread_mutex_unlock(&actual->grupo.mutex);
        actual = actual->siguiente;
    }

    // Crear nuevo grupo si no hay espacio
    GrupoNode* nuevo = malloc(sizeof(GrupoNode));
    nuevo->grupo.id = lista->grupo_id_counter++;
    nuevo->grupo.count = 1;
    nuevo->grupo.rr_index = 0;
    pthread_mutex_init(&nuevo->grupo.mutex, NULL);
    nuevo->grupo.consumidores[0].id = 0;  // El primer consumidor tiene ID 0 en el nuevo grupo
    nuevo->grupo.consumidores[0].socket_fd = socket_fd;
    nuevo->siguiente = lista->cabeza;
    lista->cabeza = nuevo;

    pthread_mutex_unlock(&lista->mutex);
    return nuevo->grupo.id;
}
void inicializar_lista() {
    lista = malloc(sizeof(ListaGrupos));
    if (lista == NULL) {
        perror("Fallo en malloc de lista");
        exit(EXIT_FAILURE);
    }
    lista->cabeza = NULL;
    lista->grupo_id_counter = 0;
    lista->consumer_id_counter = 0;
    pthread_mutex_init(&lista->mutex, NULL);
}
void* distribuir_a_grupos(void *arg) {
    Mensaje mensaje;
    Mensaje log;
    while(get_keep_running()) {
        // Esperar a que haya mensajes disponibles
        //sem_wait(&mensajes_disponibles);

        if (dequeue(&colaGlobal, &mensaje) == 0) { 
            pthread_mutex_lock(&lista->mutex);
            GrupoNode* actual = lista->cabeza;
            while (actual) {
                pthread_mutex_lock(&actual->grupo.mutex);

                if (actual->grupo.count > 0) {
                    int idx = actual->grupo.rr_index % actual->grupo.count;
                    int fd = actual->grupo.consumidores[idx].socket_fd;
                    if (send(fd, &mensaje, sizeof(Mensaje), MSG_NOSIGNAL) <= 0) {
                        // Desconexión o error
                    }
                    printf("Mensaje enviado al consumer: %s\n", mensaje.mensaje);
                    snprintf(log.mensaje, MAX_MESSAGE_LENGTH, "Mensaje ID: %d enviado al grupo %d, consumidor %d\n", mensaje.id, actual->grupo.id, actual->grupo.consumidores[idx].id);
                    agregar_mensaje(log); // Agregar el mensaje a la lista de mensajes
                    
                    actual->grupo.rr_index = (actual->grupo.rr_index + 1) % actual->grupo.count;
                }

                pthread_mutex_unlock(&actual->grupo.mutex);
                actual = actual->siguiente;
            }
            pthread_mutex_unlock(&lista->mutex);

        
        } else {
            usleep(100000);  // Espera un poco si la cola está vacía
        }
    }
    return NULL;
}
//                           SOCKETS 
// Maneja la conexi n con un cliente
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
        //consumer_conectado();  // Aumentar el contador de consumers activos
        printf("Consumer conectado\n");

        int grupo_id = registrar_consumer(client_sock);
        printf("Consumer registrado en grupo %d\n", grupo_id);

        // while (get_keep_running()) {
        //     Mensaje mensaje;

        //     // if (send(client_sock, &mensaje, sizeof(Mensaje), MSG_NOSIGNAL) <= 0) {
        //     //     printf("Consumer desconectado\n");
        //     //     break;  // Salir del bucle si el socket está cerrado
        //     // }

        //     if (dequeue(&colaGlobal, &mensaje) == 0) { 
        //         send(client_sock, &mensaje, sizeof(Mensaje), 0);
        //         printf("Mensaje enviado al consumer: %s\n", mensaje.mensaje);
        //         escribir_log(archivoLog, mensaje.mensaje);
        //         printf("Guardado en log: %s\n", mensaje.mensaje);
        //     } else {
        //         usleep(100000);  // Espera un poco si la cola está vacía
        //     }
        // }

        //consumer_desconectado();  // Disminuir el contador de consumers activos

    } else {
        // Es un producer
        Mensaje nuevoMensaje;
        while ((read_size = recv(client_sock, &nuevoMensaje, sizeof(Mensaje), 0)) > 0) {
            // nuevoMensaje.id=obtener_id_mensaje();
            printf("Mensaje recibido: %s\n", nuevoMensaje.mensaje);
            // escribir_persister(persisterFile, &nuevoMensaje);

            if (enqueue(&colaGlobal, nuevoMensaje) == -1) {
                printf("Cola llena, mensaje descartado\n");
            }
        }

        if (read_size == 0) {
            printf("Producer desconectado\n");
        } else if (read_size == -1) {
            perror("Error en recv");
        }
        close(client_sock);
    }

    return NULL;
}
void* escribir_lista_mensajes(void* arg) {
    while(get_keep_running()) {
        // Esperar a que haya mensajes disponibles
        //sem_wait(&mensajes_disponibles);
        
        Mensaje mensaje = obtener_mensaje();
        if (mensaje.id != -1) {
            escribir_log(archivoLog, mensaje.mensaje); // Escribir en el archivo de persistencia
            printf("Mensaje escrito en log: %s\n", mensaje.mensaje); // Para depuración
        }

    }
    return NULL;
}

int main() {
    struct sockaddr_in address;
    int addrlen = sizeof(address);
    pthread_t processor_thread;
    sem_init(&espacios_disponibles, 0, QUEUE_CAPACITY); // espacios disponibles al inicio
    sem_init(&mensajes_disponibles, 0, 0);
    //Inicializa la lista
    inicializar_lista();
    inicializar_lista_mensajes();

    // Inicializar la cola
    initQueue(&colaGlobal);

    // Abrir el archivo de log
    archivoLog = abrir_archivo("archivo.log");
    persisterFile = abrir_archivo("persistencia.txt");

    // Configurar el manejo de se ales
    signal(SIGINT, handle_signal);

    // Crear socket
    if ((server_fd = socket(AF_INET, SOCK_STREAM, 0)) == 0) {
        perror("Fallo en la creaci n del socket");
        exit(EXIT_FAILURE);
    }

    // Para permitir reutilizaci n del puerto
    int opt = 1;
    if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt))) {
        perror("setsockopt");
        exit(EXIT_FAILURE);
    }

    // Configurar direcci n del socket
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

    pthread_t log_thread;
    if (pthread_create(&log_thread, NULL, escribir_lista_mensajes, NULL) < 0) { //REVISAR QUE SE ESPERE A QUE TERMINEN LOS HILOS ANTES DE CERRAR EL BROKER
        perror("No se pudo crear el hilo");
    }
    else {
        // Desvincular el hilo para que se limpie autom ticamente
        pthread_detach(log_thread);
    }

    pthread_t mensajes_thread;
    if (pthread_create(&mensajes_thread, NULL, distribuir_a_grupos, NULL) < 0) { //REVISAR QUE SE ESPERE A QUE TERMINEN LOS HILOS ANTES DE CERRAR EL BROKER
        perror("No se pudo crear el hilo");
    }
    else {
        // Desvincular el hilo para que se limpie autom ticamente
        pthread_detach(mensajes_thread);
    }

    // Aceptar conexiones entrantes
    while (get_keep_running()) {
        int new_socket;
        if ((new_socket = accept(server_fd, (struct sockaddr*)&address, (socklen_t*)&addrlen)) < 0) {
            if (!get_keep_running()) break; // Si se cerr  por la se al, es normal
            perror("Fallo en accept");
            continue;
        }

        printf("Nueva conexi n aceptada\n");

        // Crear un hilo para manejar al cliente
        pthread_t client_thread;
        int* client_sock = malloc(sizeof(int));
        *client_sock = new_socket;

        if (pthread_create(&client_thread, NULL, handle_client, (void*)client_sock) < 0) { //REVISAR QUE SE ESPERE A QUE TERMINEN LOS HILOS ANTES DE CERRAR EL BROKER
            perror("No se pudo crear el hilo");
            close(new_socket);
            free(client_sock);
        }
        else {
            // Desvincular el hilo para que se limpie autom ticamente
            pthread_detach(client_thread);
        }
    }

    // Esperar a que terminen los hilos de log y mensajes
    pthread_join(log_thread, NULL);
    pthread_join(mensajes_thread, NULL);

    cerrar_archivo(persisterFile);
    cerrar_archivo(archivoLog);
    sem_destroy(&espacios_disponibles);
    sem_destroy(&mensajes_disponibles);
    close(server_fd);

    printf("Broker finalizado\n");
    return 0;
}