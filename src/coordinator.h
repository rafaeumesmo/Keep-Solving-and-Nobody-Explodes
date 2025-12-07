#ifndef COORDINATOR_H
#define COORDINATOR_H

#include <pthread.h>

// Inicializa coordenador (fila de comandos + thread)
int coord_start(void);

// Encerra coordenador (finaliza thread)
void coord_shutdown(void);

// Enfileira comandos vindos da UI (como "3f2pp", "a", "d"...)
void coord_enqueue_command(const char *cmd);

#endif
