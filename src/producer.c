//
//                     ESTA CLASE ESTA LIBRE DE MEMORY LEAKS Y DE RACE CONDITIONS
//

#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <arpa/inet.h>
//crear una estructura de mensaje para enviar el id y el mensaje
//para luego tener un cola de mensajes en el broker y que sea una cola circular


   
#define MAX_MESSAGE_LENGTH 256 // Tamaño de los msj
#define SERVER_IP "127.0.0.1"  // Esto es para poner la ip especifica del broker
#define PORT 8080              // Mismo puerto que el broker


typedef struct {
    int id;                           // ID del mensaje (puede ser ID del producer o número de secuencia)
    char mensaje[MAX_MESSAGE_LENGTH]; // Contenido del mensaje
} Mensaje;


// Establece conexion con el broker
int conectar_broker() {
    int sock=0;
    //Estructura del Socket
    struct sockaddr_in serv_addr;

    //Creamos el socket
    //AF_INET     -> Indica que es para ipv4
    //SOCK_STREAM -> Indica que es un socket TCP 
    // 0          -> Selecciona el protocolo adecuado 
    sock = socket(AF_INET, SOCK_STREAM, 0); //Si el valor almacenado en sock es mayor a 0 entonces se creo con exito
    if (sock < 0) {
        perror("Error creando el socket");
        return -1;
    }

    serv_addr.sin_family = AF_INET;   //Indican que seran socket que trabajan con ipv4
    serv_addr.sin_port = htons(PORT); //Puerto en el cual esta el server

    // Convertir direcci�n IP de texto a binario
    // esto ya que el sistema operativo necesita conocer la dir ip en binario y no cadena de texto 
    // de eso se encarga inet_pton
    if (inet_pton(AF_INET, SERVER_IP, &serv_addr.sin_addr) <= 0) {
        perror("Direcci�n inv�lida o no soportada");
        return -1;
    }

    // Conectar al broker
    // sock -> id del socket creado
    // (struct sockaddr*)&serv_addr -> es el sockaddr_in que ya tenemos pero es casteado a un sockaddr
    // sizeof(server_addr) -> es el tama�o de la estructura del socket
    if (connect(sock, (struct sockaddr*)&serv_addr, sizeof(serv_addr)) < 0) {
        perror("Error en la conexi�n");
        return -1;
    }

    printf("Conectado al broker en %s:%d\n", SERVER_IP, PORT);
    return sock;
}

// Envio del mensaje por medio del socket
int enviar_mensaje(int sock, const Mensaje* mensaje) {
    size_t len = sizeof(Mensaje); // Tamaño total del struct

    if (send(sock, mensaje, len, 0) != len) {
        perror("Error al enviar el mensaje");
        return -1;
    }
    return 0;
}


int main(int argc, char* argv[]) {

    
    int id =0;

    if (argc > 1) {
        id = atoi(argv[1]);
    } 


    int sock = conectar_broker();
    if (sock < 0) {
        return 1;
    }

    Mensaje msj = {0}; 
    msj.id=id;
    snprintf(msj.mensaje, sizeof(msj.mensaje), "Mensaje x producer %d", id);
    

    if (enviar_mensaje(sock, &msj) == 0) {
        printf("Mensaje enviado: ID=%d, contenido='%s'\n", msj.id, msj.mensaje);
    }

    close(sock);
    return 0;

}