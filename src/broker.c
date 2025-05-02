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
#define MAX_CONNECTIONS 10000
#define QUEUE_CAPACITY 1000
#define MAX_CONSUMERS_GRUPO 10
#define MAX_TAREAS 200
#define NUM_HILOS 100

sem_t espacios_disponibles;
sem_t mensajes_disponibles;
pthread_mutex_t mutexCola = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t persister_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t log_mutex= PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t mutexKeepRunning = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t mutexID = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t consumer_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t consumers_cond = PTHREAD_COND_INITIALIZER; // Condición para consumidores disponibles

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
    Consumer consumidores[MAX_CONSUMERS_GRUPO]; // <-- Cambia 5 por MAX_CONSUMERS_GRUPO
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

typedef struct NodoMensajeDinamico {
    Mensaje mensaje;
    struct NodoMensajeDinamico* siguiente;
} NodoMensajeDinamico;

typedef struct {
    NodoMensajeDinamico* cabeza;
    NodoMensajeDinamico* cola;
    pthread_mutex_t mutex;
    sem_t mensajes_disponibles;
} ColaDinamica;

typedef struct {
    int client_socket;
} Tarea;

// Nueva estructura para la lista enlazada de tareas
typedef struct NodoTarea {
    Tarea tarea;
    struct NodoTarea* siguiente;
} NodoTarea;

typedef struct {
    NodoTarea* cabeza;
    NodoTarea* cola;
    pthread_mutex_t mutex;
    sem_t tareas_disponibles;
} ListaTareas;

//----------------------------------------------------------------------------
//------------------------DECLARACION DE VARIABLES----------------------------
ListaGrupos* lista;
ListaMensajes* listaMensajes;
ColaCircular colaGlobal; //No se si deberian ser locales
ColaDinamica colaDinamica;
ColaDinamica colaLog;
ColaDinamica colaPersister;
ListaTareas lista_tareas;

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
void eliminar_consumer(int socket_fd);
void* manejar_consumer(void* socket_desc);
//     Cola Circular
void initQueue(ColaCircular* queue);
int isFull(ColaCircular* queue);
int isEmpty(ColaCircular* queue);
int enqueue(ColaCircular* queue, Mensaje mensaje);
int dequeue(ColaCircular* queue, Mensaje* mensaje);

//     Cola Dinámica
void init_cola_dinamica(ColaDinamica* cola);
void enqueue_dinamico(ColaDinamica* cola, Mensaje mensaje);
int dequeue_dinamico(ColaDinamica* cola, Mensaje* mensaje);
void liberar_cola_dinamica(ColaDinamica* cola);

void* distribuir_a_grupos(void *arg);
void* handle_client(void* socket_desc);
int get_keep_running();
void set_keep_running(int value);
void handle_signal(int sig);
void manejar_producer(int client_sock);

//      Archivos
FILE* abrir_archivo(const char* nombre_archivo);
void escribir_persister(FILE* archivo, Mensaje *mensaje);
void escribir_log(FILE* archivo, const char* mensaje);
void cerrar_archivo(FILE* archivo);
void* escribir_lista_mensajes(void* arg);

//      Thread Pool
void init_lista_tareas(ListaTareas* lista);
void agregar_tarea_lista(ListaTareas* lista, Tarea tarea);
Tarea tomar_tarea_lista(ListaTareas* lista);
void* trabajador(void* arg);

//      Hilos para log y persister
void* hilo_log(void* arg);
void* hilo_persister(void* arg);

//      Desbloquear hilos
void desbloquear_hilos_para_salida();

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
    pthread_cond_signal(&consumers_cond); // Notificar a los hilos que esperan consumidores
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
    // Espera a que haya espacio disponible (bloqueante, sin bucle activo)
    sem_wait(&espacios_disponibles);   
    pthread_mutex_lock(&mutexCola);

    if (queue->size == QUEUE_CAPACITY) {
        pthread_mutex_unlock(&mutexCola);
        sem_post(&espacios_disponibles); // Devuelve el espacio al semáforo
        return -1; // Cola llena (esto debería ser raro)
    }
    mensaje.id = obtener_id_mensaje();
    enqueue_dinamico(&colaPersister, mensaje);

    queue->mensajes[queue->rear] = mensaje;
    queue->rear = (queue->rear + 1) % QUEUE_CAPACITY;
    queue->size++;

    pthread_mutex_unlock(&mutexCola);
    sem_post(&mensajes_disponibles);
    return 0;
}
int dequeue(ColaCircular* queue, Mensaje* mensaje) {
    sem_wait(&mensajes_disponibles);  
    pthread_mutex_lock(&consumer_mutex);
        while (consumers_activos == 0) {
            pthread_cond_wait(&consumers_cond, &consumer_mutex); // Esperar a que haya consumidores
        }
    pthread_mutex_unlock(&consumer_mutex);

    pthread_mutex_lock(&mutexCola);  // Bloquear el mutex al comenzar
    if (queue->size == 0 || hay_consumers() == 0) {
        printf("Cola vacía o no hay consumidores\n");
        pthread_mutex_unlock(&mutexCola);  // Desbloquear antes de salir
        sem_post(&mensajes_disponibles); 
        return -1; // Cola vacía
    }

    *mensaje = queue->mensajes[queue->front];
    queue->front = (queue->front + 1) % QUEUE_CAPACITY;
    queue->size--;

    pthread_mutex_unlock(&mutexCola);  // Desbloquear al finalizar
    sem_post(&espacios_disponibles);   
    return 0;
}
void init_cola_dinamica(ColaDinamica* cola) {
    cola->cabeza = cola->cola = NULL;
    pthread_mutex_init(&cola->mutex, NULL);
    sem_init(&cola->mensajes_disponibles, 0, 0);
}

void enqueue_dinamico(ColaDinamica* cola, Mensaje mensaje) {
    NodoMensajeDinamico* nuevo = malloc(sizeof(NodoMensajeDinamico));
    if (!nuevo) return;
    nuevo->mensaje = mensaje;
    nuevo->siguiente = NULL;

    pthread_mutex_lock(&cola->mutex);
    if (cola->cola) {
        cola->cola->siguiente = nuevo;
        cola->cola = nuevo;
    } else {
        cola->cabeza = cola->cola = nuevo;
    }
    pthread_mutex_unlock(&cola->mutex);
    sem_post(&cola->mensajes_disponibles);
}

int dequeue_dinamico(ColaDinamica* cola, Mensaje* mensaje) {
    sem_wait(&cola->mensajes_disponibles);
    pthread_mutex_lock(&cola->mutex);
    NodoMensajeDinamico* nodo = cola->cabeza;
    if (!nodo) {
        pthread_mutex_unlock(&cola->mutex);
        return -1;
    }
    *mensaje = nodo->mensaje;
    cola->cabeza = nodo->siguiente;
    if (!cola->cabeza) cola->cola = NULL;
    free(nodo);
    pthread_mutex_unlock(&cola->mutex);
    return 0;
}

void liberar_cola_dinamica(ColaDinamica* cola) {
    pthread_mutex_lock(&cola->mutex);
    NodoMensajeDinamico* actual = cola->cabeza;
    while (actual) {
        NodoMensajeDinamico* tmp = actual;
        actual = actual->siguiente;
        free(tmp);
    }
    pthread_mutex_unlock(&cola->mutex);
    pthread_mutex_destroy(&cola->mutex);
    sem_destroy(&cola->mensajes_disponibles);
}

//                    ARCHIVOS .LOG Y PERSISTER 
FILE* abrir_archivo(const char* nombre_archivo) {
    FILE* archivo = fopen(nombre_archivo, "w");//Abrimos en modo "w" para crear el archivo o sobrescribirlo
    if (archivo == NULL) {
        perror("No se pudo abrir el archivo");
        exit(1);  // Termina el programa si no puede abrir el archivo
    }
    fclose(archivo); // Cerrar el archivo inmediatamente para asegurarnos de que se cree

    archivo = fopen(nombre_archivo, "a");  // Modo "a" para agregar al archivo sin sobrescribir
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
// Maneja la señal para terminar el programa
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
            consumer_conectado(); 
            pthread_mutex_unlock(&lista->mutex);
            return grupo_id;
        }
        pthread_mutex_unlock(&actual->grupo.mutex);
        actual = actual->siguiente;
    }

    // Crear nuevo grupo si no hay espacio
    GrupoNode* nuevo = calloc(1, sizeof(GrupoNode));
    nuevo->grupo.id = lista->grupo_id_counter++;
    nuevo->grupo.count = 1;
    nuevo->grupo.rr_index = 0;
    pthread_mutex_init(&nuevo->grupo.mutex, NULL);
    nuevo->grupo.consumidores[0].id = 0;  // El primer consumidor tiene ID 0 en el nuevo grupo
    nuevo->grupo.consumidores[0].socket_fd = socket_fd;
    nuevo->siguiente = lista->cabeza;
    lista->cabeza = nuevo;

    consumer_conectado(); 
    pthread_mutex_unlock(&lista->mutex);
    
    return nuevo->grupo.id;
}
void eliminar_consumer(int socket_fd) {
    pthread_mutex_lock(&lista->mutex);

    GrupoNode* actual = lista->cabeza;
    while (actual) {
        pthread_mutex_lock(&actual->grupo.mutex);

        for (int i = 0; i < actual->grupo.count; i++) {
            if (actual->grupo.consumidores[i].socket_fd == socket_fd) {
                // Desplazar los consumidores restantes para llenar el hueco
                for (int j = i; j < actual->grupo.count - 1; j++) {
                    actual->grupo.consumidores[j] = actual->grupo.consumidores[j + 1];
                }
                actual->grupo.count--; // Reducir el contador de consumidores

                // --- AJUSTE DE rr_index ---
                if (actual->grupo.count == 0) {
                    actual->grupo.rr_index = 0;
                } else if (actual->grupo.rr_index > i) {
                    actual->grupo.rr_index--;
                } else if (actual->grupo.rr_index >= actual->grupo.count) {
                    actual->grupo.rr_index = 0;
                }
                // --------------------------

                printf("Consumer eliminado del grupo %d\n", actual->grupo.id);
                
                if (actual->grupo.count == 0) {
                    // Eliminar el grupo de la lista
                    if (lista->cabeza == actual) {
                        lista->cabeza = actual->siguiente;
                    } else {
                        GrupoNode* prev = lista->cabeza;
                        while (prev && prev->siguiente != actual) prev = prev->siguiente;
                        if (prev) prev->siguiente = actual->siguiente;
                    }
                    pthread_mutex_unlock(&actual->grupo.mutex);
                    pthread_mutex_destroy(&actual->grupo.mutex);
                    free(actual);
                    consumer_desconectado();
                    pthread_mutex_unlock(&lista->mutex);
                    return;
                }

                pthread_mutex_unlock(&actual->grupo.mutex);
                consumer_desconectado();
                pthread_mutex_unlock(&lista->mutex);
                return;
            }
        }
        
        pthread_mutex_unlock(&actual->grupo.mutex);
        actual = actual->siguiente;
    }
    pthread_mutex_unlock(&lista->mutex);
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
    while (1) {
        if (!get_keep_running()) break;
        if (dequeue_dinamico(&colaDinamica, &mensaje) == 0 && get_keep_running()) {
            pthread_mutex_lock(&lista->mutex);
            GrupoNode* actual = lista->cabeza;
            while (actual) {
                pthread_mutex_lock(&actual->grupo.mutex);
                if (actual->grupo.count > 0) {
                    int idx = actual->grupo.rr_index % actual->grupo.count;
                    int fd = actual->grupo.consumidores[idx].socket_fd;
                    if (send(fd, &mensaje, sizeof(Mensaje), MSG_NOSIGNAL) <= 0) {
                        eliminar_consumer(fd);
                        enqueue_dinamico(&colaDinamica, mensaje); // Reenviar el mensaje
                    }
                    actual->grupo.rr_index = (actual->grupo.rr_index + 1) % actual->grupo.count;
                }
                pthread_mutex_unlock(&actual->grupo.mutex);
                actual = actual->siguiente;
            }
            pthread_mutex_unlock(&lista->mutex);
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
   // free(socket_desc);

    char tipo[4] = {0};  // Buffer para verificar si es "GET"

    int read_size = recv(client_sock, tipo, 3, MSG_PEEK);  // Leer sin consumir
    if (read_size <= 0) {
        close(client_sock);
        return NULL;
    }

    if (strncmp(tipo, "GET", 3) == 0) {
        // Es un consumer
        // consumer → hilo dedicado
        pthread_t hilo_consumer;
        int* socket_cpy = malloc(sizeof(int));
        *socket_cpy = client_sock;
        pthread_create(&hilo_consumer, NULL, manejar_consumer, socket_cpy);
        pthread_detach(hilo_consumer);  // o podés guardarlos si querés join al final
    } else {
        // Es un producer
        // producer → manejar directamente aquí
        manejar_producer(client_sock);
    }

    return NULL;
}

void* manejar_consumer(void* socket_desc) {
    int client_sock = *(int*)socket_desc;
    free(socket_desc);
    // ... tu lógica actual del consumer
    char dummy[4];
        recv(client_sock, dummy, 3, 0);  // Consumimos los 3 bytes del "GET"
        printf("Consumer conectado\n");

        int grupo_id = registrar_consumer(client_sock);
       
        printf("Consumer registrado en grupo %d\n", grupo_id);

        while (get_keep_running()) {
            Mensaje mensaje;
            if (recv(client_sock, &mensaje, sizeof(Mensaje), MSG_PEEK) <= 0) {
                printf("Consumer desconectado\n");
                eliminar_consumer(client_sock); // Eliminar el consumidor del grupo
                break;
            }
            usleep(100000); // Simulación de espera
        }

        close(client_sock);
    return NULL;
}

void manejar_producer(int client_sock) {
    Mensaje nuevoMensaje;
    int read_size; 
    while ((read_size = recv(client_sock, &nuevoMensaje, sizeof(Mensaje), 0)) > 0) {
        nuevoMensaje.id = obtener_id_mensaje();
        enqueue_dinamico(&colaDinamica, nuevoMensaje);

        // Encolar para log y persistencia asíncrona
        enqueue_dinamico(&colaLog, nuevoMensaje);
        enqueue_dinamico(&colaPersister, nuevoMensaje);
    }
    close(client_sock);
}
void* escribir_lista_mensajes(void* arg) {
    while(get_keep_running()) {
        // Esperar a que haya mensajes disponibles
        //sem_wait(&mensajes_disponibles);
        
        Mensaje mensaje = obtener_mensaje();
        if (mensaje.id != -1) {
            enqueue_dinamico(&colaLog, mensaje); // Agregar el mensaje a la cola de log
            printf("Mensaje escrito en log: %s\n", mensaje.mensaje); // Para depuración
        }

    }
    return NULL;
}

void init_lista_tareas(ListaTareas* lista) {
    lista->cabeza = NULL;
    lista->cola = NULL;
    pthread_mutex_init(&lista->mutex, NULL);
    sem_init(&lista->tareas_disponibles, 0, 0);
}

void agregar_tarea_lista(ListaTareas* lista, Tarea tarea) {
    NodoTarea* nuevo = malloc(sizeof(NodoTarea));
    if (!nuevo) return;
    nuevo->tarea = tarea;
    nuevo->siguiente = NULL;

    pthread_mutex_lock(&lista->mutex);
    if (lista->cola) {
        lista->cola->siguiente = nuevo;
        lista->cola = nuevo;
    } else {
        lista->cabeza = lista->cola = nuevo;
    }
    pthread_mutex_unlock(&lista->mutex);
    sem_post(&lista->tareas_disponibles);
}

Tarea tomar_tarea_lista(ListaTareas* lista) {
    sem_wait(&lista->tareas_disponibles);
    pthread_mutex_lock(&lista->mutex);
    NodoTarea* nodo = lista->cabeza;
    Tarea tarea = { .client_socket = -1 };
    if (nodo) {
        tarea = nodo->tarea;
        lista->cabeza = nodo->siguiente;
        if (!lista->cabeza) lista->cola = NULL;
        free(nodo);
    }
    pthread_mutex_unlock(&lista->mutex);
    return tarea;
}

void* trabajador(void* arg) {
    while (1) {
        if (!get_keep_running()) break;
        Tarea tarea = tomar_tarea_lista(&lista_tareas);
        if (tarea.client_socket != -1 && get_keep_running()) {
            handle_client((void*)&tarea.client_socket);
        }
    }
    return NULL;
}

void* hilo_log(void* arg) {
    while (1) {
        if (!get_keep_running()) break;
        Mensaje mensaje;
        if (dequeue_dinamico(&colaLog, &mensaje) == 0 && get_keep_running()) {
            escribir_log(archivoLog, mensaje.mensaje);
        }
    }
    return NULL;
}

void* hilo_persister(void* arg) {
    while (1) {
        if (!get_keep_running()) break;
        Mensaje mensaje;
        if (dequeue_dinamico(&colaPersister, &mensaje) == 0 && get_keep_running()) {
            escribir_persister(persisterFile, &mensaje);
        }
    }
    return NULL;
}

void desbloquear_hilos_para_salida() {
    // Despertar hilos del thread pool
    for (int i = 0; i < NUM_HILOS; i++) {
        sem_post(&lista_tareas.tareas_disponibles);
    }
    // Despertar hilos de log, persister y mensajes
    sem_post(&colaLog.mensajes_disponibles);
    sem_post(&colaPersister.mensajes_disponibles);
    sem_post(&colaDinamica.mensajes_disponibles);
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

    // Inicializar la cola dinámica
    init_cola_dinamica(&colaDinamica);
    init_cola_dinamica(&colaLog);
    init_cola_dinamica(&colaPersister);

    pthread_t log_thread, persister_thread;
    pthread_create(&log_thread, NULL, hilo_log, NULL);
    pthread_create(&persister_thread, NULL, hilo_persister, NULL);

    // Inicializar la lista de tareas
    init_lista_tareas(&lista_tareas);

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

    pthread_t pool[NUM_HILOS];
    for (int i = 0; i < NUM_HILOS; i++) {
        pthread_create(&pool[i], NULL, trabajador, NULL);
     }

    printf("Broker iniciado en puerto %d\n", PORT);

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

        int* client_sock = malloc(sizeof(int));
        *client_sock = new_socket;
        
        // Revisar el tipo de cliente
        char tipo[4] = {0};
        int read_size = recv(new_socket, tipo, 3, MSG_PEEK);
        if (read_size <= 0) {
            close(new_socket);
            free(client_sock);
            continue;
        }
        
        if (strncmp(tipo, "GET", 3) == 0) {
            // Es un consumer → hilo dedicado
            pthread_t consumer_thread;
            if (pthread_create(&consumer_thread, NULL, manejar_consumer, client_sock) < 0) {
                perror("No se pudo crear hilo para consumer");
                close(new_socket);
                free(client_sock);
            } else {
                pthread_detach(consumer_thread);
            }
        } else {
            // Es un producer → tarea para el thread pool (lista ilimitada)
            Tarea tarea = { .client_socket = new_socket };
            agregar_tarea_lista(&lista_tareas, tarea);
            free(client_sock); // <-- Agrega esta línea aquí
        }
        
    }

    // Desbloquear hilos para salida
    desbloquear_hilos_para_salida();

    // Esperar a que terminen los hilos de log y mensajes
    pthread_join(log_thread, NULL);
    pthread_join(persister_thread, NULL);
    //pthread_join(mensajes_thread, NULL);

    sem_destroy(&lista_tareas.tareas_disponibles);
    pthread_mutex_destroy(&lista_tareas.mutex);

    liberar_cola_dinamica(&colaDinamica);
    liberar_cola_dinamica(&colaLog);
    liberar_cola_dinamica(&colaPersister);

    cerrar_archivo(persisterFile);
    cerrar_archivo(archivoLog);
    sem_destroy(&espacios_disponibles);
    sem_destroy(&mensajes_disponibles);
    close(server_fd);

    printf("Broker finalizado\n");
    return 0;
}
