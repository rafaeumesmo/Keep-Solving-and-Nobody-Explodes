#define _POSIX_C_SOURCE 200809L
#include "coordinator.h"
#include "mural.h"
#include "tedax.h"
#include "ui.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <ctype.h>
#include <unistd.h>

#define CMD_MAX 64
#define QUEUE_SIZE 64

// ======= FILA DE COMANDOS =======
static char queue[QUEUE_SIZE][CMD_MAX];
static int q_head = 0;
static int q_tail = 0;

static pthread_mutex_t q_mut = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  q_cond = PTHREAD_COND_INITIALIZER;

static pthread_t coord_thread;
static int running = 1;

// ======================================================
// Enfileira comandos chamados pela UI
// ======================================================
void coord_enqueue_command(const char *cmd) {
    pthread_mutex_lock(&q_mut);
    int next = (q_tail + 1) % QUEUE_SIZE;
    if (next == q_head) {
        // fila cheia -> descarta comando
        pthread_mutex_unlock(&q_mut);
        return;
    }
    strncpy(queue[q_tail], cmd, CMD_MAX);
    queue[q_tail][CMD_MAX-1] = '\0';
    q_tail = next;

    pthread_cond_signal(&q_cond);
    pthread_mutex_unlock(&q_mut);
}

// ======================================================
// Desenfileirar COMANDO
// ======================================================
static int dequeue(char *out) {
    pthread_mutex_lock(&q_mut);

    while (running && q_head == q_tail) {
        pthread_cond_wait(&q_cond, &q_mut);
    }
    if (!running) {
        pthread_mutex_unlock(&q_mut);
        return 0;
    }

    strncpy(out, queue[q_head], CMD_MAX);
    q_head = (q_head + 1) % QUEUE_SIZE;

    pthread_mutex_unlock(&q_mut);
    return 1;
}

// ======================================================
// TRATAR COMANDOS
// ======================================================

static void handle_auto_assign() {
    module_t *m = mural_pop();
    if (!m) {
        log_event("[COORD] Nenhum modulo para atribuir.");
        return;
    }
    int bench = tedax_request_auto(m);
    if (bench < 0) {
        log_event("[COORD] Nenhuma bancada disponivel para M%d", m->id);
        mural_requeue(m);
    } else {
        log_event("[COORD] M%d enviado automaticamente para bancada %d", m->id, bench);
    }
}

static void handle_manual(const char *cmd) {
    // Formato esperado:
    //   <tedax><tipo><bancada><presses...>
    // Exemplo:
    //   3f2pp
    //   T=3 tipo=f bancada=2 presses=pp
    int len = strlen(cmd);
    if (len < 4) {
        log_event("[COORD] comando manual invalido: %s", cmd);
        return;
    }

    int t = cmd[0] - '0';
    char type = cmd[1];
    int b = cmd[2] - '0';
    int presses = len - 3;

    // encontrar modulo correspondente no mural
    module_t *m = mural_find_by_tedax_type(t, type);
    if (!m) {
        log_event("[COORD] Nenhum modulo combina com %s", cmd);
        return;
    }

    // tentar enviar ao tedax
    int ok = tedax_request_manual(m, t, b, presses);
    if (!ok) {
        log_event("[COORD] Bancada %d ocupada ou invalida.", b);
        mural_requeue(m);
    } else {
        log_event("[COORD] M%d enviado manualmente p/ T%d B%d (%d presses)",
                  m->id, t, b, presses);
    }
}

// ======================================================
// THREAD DO COORDENADOR
// ======================================================
static void* coordinator_fn(void *arg) {
    (void)arg;

    char cmd[CMD_MAX];

    while (running) {
        if (!dequeue(cmd))
            continue;

        if (strcmp(cmd, "a") == 0) {
            handle_auto_assign();
        }
        else if (strcmp(cmd, "q") == 0) {
            running = 0;
            break;
        }
        else {
            handle_manual(cmd);
        }
    }
    return NULL;
}

// ======================================================
// PUBLIC FUNCTIONS
// ======================================================

int coord_start(void) {
    running = 1;
    if (pthread_create(&coord_thread, NULL, coordinator_fn, NULL) != 0) {
        perror("pthread_create");
        return 1;
    }
    return 0;
}

void coord_shutdown(void) {
    running = 0;
    pthread_cond_broadcast(&q_cond);
    pthread_join(coord_thread, NULL);
}
