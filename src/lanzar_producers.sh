#!/bin/bash

# Número de productores que querés lanzar
NUM_PRODUCERS=50

# Ruta al ejecutable de tu producer
PRODUCER_EXEC=./producer  # Cambiá el nombre si tu ejecutable se llama diferente

for i in $(seq 1 $NUM_PRODUCERS)
do
    echo "Lanzando Producer $i"
    $PRODUCER_EXEC $i &
done

echo "Todos los Producers fueron lanzados."
