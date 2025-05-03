#!/bin/bash

# Número de consumidores que querés lanzar
NUM_CONSUMERS=1000

# Ruta al ejecutable de tu consumer
CONSUMER_EXEC=./consumer  # Cambiá el nombre si tu ejecutable se llama diferente

for i in $(seq 1 $NUM_CONSUMERS)
do
    echo "Lanzando Consumer $i"
    $CONSUMER_EXEC &
done

echo "Todos los Consumers fueron lanzados."
