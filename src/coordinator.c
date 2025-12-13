#define _POSIX_C_SOURCE 200809L
#include "coordinator.h"
#include "mural.h"
#include "tedax.h"
#include "ui.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>

#define CMD_MAX 128
#define QUEUE_SIZE 64

static char queue[QUEUE_SIZE][CMD_MAX];
static int q_head = 0;
static int q_tail = 0;
static pthread_mutex_t q_mut = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  q_cond = PTHREAD_COND_INITIALIZER;
static pthread_t coord_thread;
static int running = 1;

void coord_enqueue_command(const char *cmd) {
    pthread_mutex_lock(&q_mut);
    int next = (q_tail + 1) % QUEUE_SIZE;
    if (next != q_head) {
        strncpy(queue[q_tail], cmd, CMD_MAX);
        queue[q_tail][CMD_MAX-1] = '\0';
        q_tail = next;
        pthread_cond_signal(&q_cond);
    }
    pthread_mutex_unlock(&q_mut);
}

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

static void handle_auto_assign_generic() {
    module_t *m = mural_pop();
    if (!m) { log_event("[COORD] Mural vazio!"); return; }
    int tid = tedax_request_auto(m);
    if (tid < 0) {
        log_event("[COORD] Sem recursos p/ M%d. Re-enfileirado.", m->id);
        mural_requeue(m);
    } else {
        log_event("[COORD] Auto: M%d -> T%d", m->id, tid);
    }
}

static void handle_manual_assign(const char *cmd) {
    int m_id, t_id, b_id; char instr[64];
    int parsed = 0;
    if (sscanf(cmd, "M %d %d %d %[^\n]", &m_id, &t_id, &b_id, instr) == 4) parsed = 1;
    else if (sscanf(cmd, "M %d %[^\n]", &m_id, instr) == 2) parsed = 0;
    else { log_event("[COORD] Erro comando: %s", cmd); return; }

    module_t *m = mural_pop_by_id(m_id);
    if (!m) { log_event("[COORD] Falha: M%d nao existe.", m_id); return; }
    snprintf(m->instruction, sizeof(m->instruction), "%s", instr);

    if (parsed) {
        if (!tedax_request_manual(m, t_id, b_id, 0)) {
            log_event("[COORD] Falha atribuir M%d. Re-enfileirado.", m_id);
            mural_requeue(m);
        }
    } else {
        if (tedax_request_auto(m) < 0) {
            log_event("[COORD] M%d sem recursos. Re-enfileirado.", m_id);
            mural_requeue(m);
        }
    }
}

static void* coordinator_fn(void *arg) {
    (void)arg; char cmd[CMD_MAX];
    while (running) {
        if (!dequeue(cmd)) continue;
        if (cmd[0] == 'A' || cmd[0] == 'a') handle_auto_assign_generic();
        else if (cmd[0] == 'M') handle_manual_assign(cmd);
        else if (cmd[0] == 'Q') { running = 0; break; }
        else log_event("[COORD] Desconhecido: %s", cmd);
    }
    return NULL;
}

int coord_start(void) {
    // Reset da fila ao iniciar
    q_head = 0; q_tail = 0;
    running = 1;
    if (pthread_create(&coord_thread, NULL, coordinator_fn, NULL) != 0) return 1;
    return 0;
}

void coord_shutdown(void) {
    running = 0;
    pthread_cond_broadcast(&q_cond);
    pthread_join(coord_thread, NULL);
}