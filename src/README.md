
# Sistema de Mensajería en C (Broker, Producer y Consumer)

## Descripción del sistema

Este proyecto implementa un sistema básico de mensajería en C utilizando hilos (threads) y comunicación a través de sockets.
El sistema está compuesto por tres componentes principales:

- **Broker**: Actúa como intermediario entre productores y consumidores. 
Recibe mensajes de los productores y los entrega a los consumidores según una estrategia Round Robin, 
asegurando que solo un consumidor por grupo consuma cada mensaje. Los mensajes se almacenan en una cola circular principal con soporte para persistencia 
y se distribuyen de forma eficiente usando un Thread Pool, semáforos y variables de condición.
El broker puede manejar un cierto número de conexiones simultáneas y registra cada mensaje en un archivo de log para llevar un registro de cuales mensajes se han 
mandado y quien los ha recibido.

- **Producer**: Envía mensajes al broker simulando la producción de datos. Los mensajes pueden ser enviados desde varios productores 
concurrentemente. En caso de saturación, los productores esperan mediante semáforos hasta que haya espacio disponible.

- **Consumer**: Se conecta al broker para recibir mensajes. Los consumidores están organizados por grupos (máximo 10 por grupo), 
y el broker gestiona la entrega para que solo un consumidor por grupo reciba cada mensaje. Además, se utiliza una variable de
condición para gestionar eficientemente la notificación a los consumidores disponibles.

## Instrucciones para compilar y ejecutar

1. **Compilar los componentes**
   Para compilar los componentes existen dos maneras:

   a. Compilar archivo por archivo de la siguienta forma
      ```bash
      gcc -o broker broker.c -lpthread
      gcc -o producer producer.c
      gcc -o consumer consumer.c
      ```

   b. Utilizar el Makefile:
      ```bash
      make
      ```

2. **Ejecutar el broker**

   ```bash
   ./broker
   ```

   El broker escucha conexiones entrantes desde productores y consumidores.

3. **Ejecutar uno o más productores**

   ```bash
   ./producer <ID_PRODUCTOR>(El id es opcional)
   ```

   Ejemplo:

   ```bash
   ./producer 1
   ```

4. **Ejecutar uno o más consumidores**

   ```bash
   ./consumer
   ```

   Ejemplo:

   ```bash
   ./consumer
   ```

## Estrategia para evitar interbloqueos

El sistema implementa varias estrategias para evitar condiciones de interbloqueo:

- **Mutexes bien definidos**: Se utilizan mutexes para proteger el acceso concurrente a la cola de mensajes, 
lista de consumidores y archivo de logs. Cada mutex protege una estructura de datos específica.

- **Orden consistente de adquisición**: Los mutexes se adquieren en un orden consistente (por ejemplo, primero el de la cola, 
luego el de los consumidores si se requiere), lo cual evita ciclos de espera circulares.

- **Semáforos**: Se usan para controlar la disponibilidad de espacios en la cola (espacios_disponibles) y 
la presencia de mensajes (mensajes_disponibles), previniendo bloqueos activos.

 
- **Round Robin por grupo**: El envío de mensajes a consumidores se realiza por turnos entre los miembros de un grupo,
 reduciendo la posibilidad de espera prolongada o bloqueo entre consumidores.

## Problemas conocidos o limitaciones

- **Tamaño fijo de grupos**: Cada grupo de consumidores puede tener como máximo 10 consumidores. 
Esta limitación está codificada y no es configurable dinámicamente.

- **Capacidad de la cola**: La cola circular principal tiene una capacidad fija (QUEUE_CAPACITY). 
Aumentar esta capacidad puede requerir mayor consumo de memoria.
<!-- 
- **Escalabilidad limitada**: Aunque el diseño soporta múltiples conexiones, no está optimizado para alta concurrencia 
o grandes volúmenes de mensajes, su limite de consumidores es de 1000 y de productores es de 7500. -->

- **Límite del sistema en descriptores de archivos abiertos:**  
  Debido a la naturaleza concurrente del broker, que acepta múltiples conexiones mediante sockets, es necesario aumentar el límite de descriptores de archivos abiertos del sistema. Si no se hace, puede aparecer el siguiente error:

   Fallo en accept: Too many open files

   Para evitarlo, se recomienda ejecutar lo siguiente **antes de iniciar el broker**:

   ```bash
   ulimit -n num_max_archivos
   ```

   Ejemplo:

   ```bash
   ulimit -n num_max_archivos
   ```

   Tenga en cuenta que se interpretan como archivos abiertos:
   - Los archivos .log y .txt que se utilizan para la persistencia que maneja el broker.
   - Cada descriptor de terminal que tenga abierto.
   - Cada socket TCP utilizado.

   Por lo tanto, si por ejemplo desea enviar 2000 producers a la vez, teniendo una terminal abierta para el broker, una para los consumers y una para enviar los producers, debería configurar un máximo de conexiones de la siguiente manera (2000 para los producers, 2 para los archivos y 3 para las terminales):
   ```bash
   ulimit -n 2005
   ```