#ifndef TEDAX_H
#define TEDAX_H

#define _POSIX_C_SOURCE 200809L

#include <pthread.h>
#include <semaphore.h>
#include "mural.h"

// Estrutura do TEDAX (deve corresponder ao que tedax.c usa)
typedef struct tedax {
    int id;
    int busy;               // 0 free, 1 processing
    module_t *current;      // módulo atualmente sendo processado (propriedade durante o processamento)
    int bench_id;           // bancada atribuída (-1 se nenhuma)
    pthread_t thr;
    pthread_mutex_t lock;
    pthread_cond_t cond;
    time_t start_time;
    int remaining;
} tedax_t;

// lifecycle
void tedax_pool_init(int n, int benches_count, sem_t *benches_sem);
void tedax_pool_shutdown(void);
void tedax_pool_destroy(void);

// APIs usadas por coordinator / ui / mural
int tedax_assign_module(int id, module_t *m); // assign to specific tedax id
int tedax_request_auto(module_t *m);           // coordinator : auto-assign -> returns tedax id or -1
int tedax_request_manual(module_t *m, int tedax_id, int bench_id, int presses); // manual assign (returns 1 ok / 0 fail)

tedax_t* tedax_get(int id);
int tedax_count(void);
int tedax_bench_count(void);

#endif // TEDAX_H
