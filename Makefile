# Compilador
CC = gcc

# Opciones para cada ejecutable
BROKER_FLAGS = -lpthread
PRODUCER_FLAGS =
CONSUMER_FLAGS =

# Archivos que se van a compilar
BROKER_SRC = src/broker.c
PRODUCER_SRC = src/producer.c
CONSUMER_SRC = src/consumer.c

# Nombres de los ejecutables
all: broker producer consumer

broker: $(BROKER_SRC)
	$(CC) -o broker $(BROKER_SRC) $(BROKER_FLAGS)

producer: $(PRODUCER_SRC)
	$(CC) -o producer $(PRODUCER_SRC) $(PRODUCER_FLAGS)

consumer: $(CONSUMER_SRC)
	$(CC) -o consumer $(CONSUMER_SRC) $(CONSUMER_FLAGS)

# Para limpiar los archivos generados (Eliminar ejecutables)
clean:
	rm -f broker producer consumer
