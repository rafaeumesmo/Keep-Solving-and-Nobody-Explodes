#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <time.h>

#include "mural.h"
#include "ui.h"
#include "config.h"

// Duas listas: Ativos e Resolvidos
static module_t *mural_head = NULL;
static module_t *mural_tail = NULL;

static module_t *resolved_head = NULL; // Lista de resolvidos

pthread_mutex_t mural_lock = PTHREAD_MUTEX_INITIALIZER;
static int mural_size = 0;
static int global_score = 0;
static int global_money = MOEDAS_INICIAL;
static time_t game_deadline = 0; 

// =====================================================
//  Criação de Módulos
// =====================================================
module_t* create_module(int id) {
    module_t *m = calloc(1, sizeof(module_t));
    if (!m) return NULL;

    m->id = id;
    m->type = rand() % 3;
    // Time required varies by module type (in seconds)
    switch (m->type) {
        case MOD_FIOS:  m->time_required = 8;  break; // fios: rapido
        case MOD_BOTAO: m->time_required = 12; break; // botao: medio
        case MOD_SENHAS: m->time_required = 18; break; // senhas: mais longo
        default: m->time_required = 10; break;
    }
    m->created_at = time(NULL);
    m->timeout_secs = 20 + rand() % 10;
    m->instruction[0] = '\0';

    switch (m->type) {
        case MOD_FIOS: {
            int correct = (rand() % 3) + 1;
            snprintf(m->solution, sizeof(m->solution), "CUT %d", correct);
        } break;
        case MOD_BOTAO: {
            const char *colors[] = {"RED", "BLUE", "GREEN", "YELLOW"};
            const char *actions[] = {"HOLD", "PRESS", "DOUBLE"};
            int c = rand() % 4;
            int a = rand() % 3;
            snprintf(m->solution, sizeof(m->solution), "%s %s", colors[c], actions[a]);
        } break;
        case MOD_SENHAS: {
            const char *words[] = {"FIRE", "WATER", "EARTH", "WIND", "VOID"};
            snprintf(m->solution, sizeof(m->solution), "WORD %s", words[rand() % 5]);
        } break;
    }
    return m;
}

// =====================================================
//  Gerenciamento da Fila (ATIVOS)
// =====================================================
void mural_push(module_t *m) {
    pthread_mutex_lock(&mural_lock);
    m->next = NULL; // Garante que não aponta para lixo
    if (!mural_head) { mural_head = mural_tail = m; } 
    else { mural_tail->next = m; mural_tail = m; }
    mural_size++;
    log_event("[MURAL] M%d adicionado", m->id);
    pthread_mutex_unlock(&mural_lock);
}

module_t* mural_pop_front(void) {
    pthread_mutex_lock(&mural_lock);
    if (!mural_head) { pthread_mutex_unlock(&mural_lock); return NULL; }
    module_t *m = mural_head;
    mural_head = mural_head->next;
    if (!mural_head) mural_tail = NULL;
    m->next = NULL;
    mural_size--;
    pthread_mutex_unlock(&mural_lock);
    return m;
}

module_t* mural_pop(void) { return mural_pop_front(); }

module_t* mural_pop_by_id(int id) {
    pthread_mutex_lock(&mural_lock);
    module_t *cur = mural_head;
    module_t *prev = NULL;
    while (cur) {
        if (cur->id == id) {
            if (prev) prev->next = cur->next;
            else mural_head = cur->next;
            if (cur == mural_tail) mural_tail = prev;
            cur->next = NULL;
            mural_size--;
            pthread_mutex_unlock(&mural_lock);
            return cur;
        }
        prev = cur;
        cur = cur->next;
    }
    pthread_mutex_unlock(&mural_lock);
    return NULL;
}

void mural_requeue(module_t *m) {
    if (!m) return;
    pthread_mutex_lock(&mural_lock);
    m->next = NULL;
    if (!mural_head) { mural_head = mural_tail = m; } 
    else { mural_tail->next = m; mural_tail = m; }
    mural_size++;
    log_event("[MURAL] M%d re-enfileirado", m->id);
    pthread_mutex_unlock(&mural_lock);
}

module_t* mural_peek_list(void) { return mural_head; }
int mural_count(void) { return mural_size; }

module_t* mural_find_by_tedax_type(int tedax_id, char type_char) {
    (void)tedax_id; 
    pthread_mutex_lock(&mural_lock);
    module_t *cur = mural_head;
    while (cur) {
        char cur_type_char = (cur->type == MOD_FIOS) ? 'F' : (cur->type == MOD_BOTAO) ? 'B' : (cur->type == MOD_SENHAS) ? 'S' : '?';
        if (cur_type_char == type_char) {
            pthread_mutex_unlock(&mural_lock);
            return cur;
        }
        cur = cur->next;
    }
    pthread_mutex_unlock(&mural_lock);
    return NULL;
}

module_t* mural_get_by_index(int index) {
    pthread_mutex_lock(&mural_lock);
    module_t *cur = mural_head;
    int i = 0;
    while (cur && i < index) { cur = cur->next; i++; }
    pthread_mutex_unlock(&mural_lock);
    return cur; 
}

// =====================================================
//  Gestão de Resolvidos (NOVO)
// =====================================================
void mural_add_to_resolved(module_t *m) {
    if (!m) return;
    pthread_mutex_lock(&mural_lock);
    // Insere no início da lista (Pilha) para ver os mais recentes primeiro
    m->next = resolved_head;
    resolved_head = m;
    pthread_mutex_unlock(&mural_lock);
}

module_t* mural_peek_resolved(void) {
    return resolved_head;
}

// =====================================================
//  Init / Destroy / Utils
// =====================================================
void mural_init(void) {
    pthread_mutex_lock(&mural_lock);
    mural_head = mural_tail = NULL;
    resolved_head = NULL;
    mural_size = 0;
    global_score = 0;
    global_money = MOEDAS_INICIAL;
    game_deadline = 0; 
    pthread_mutex_unlock(&mural_lock);
}

void mural_destroy(void) {
    pthread_mutex_lock(&mural_lock);
    // Limpa ativos
    module_t *cur = mural_head;
    while (cur) {
        module_t *n = cur->next;
        free(cur);
        cur = n;
    }
    // Limpa resolvidos
    cur = resolved_head;
    while (cur) {
        module_t *n = cur->next;
        free(cur);
        cur = n;
    }
    mural_head = mural_tail = NULL;
    resolved_head = NULL;
    mural_size = 0;
    pthread_mutex_unlock(&mural_lock);
}

void mural_lock_access(void) { pthread_mutex_lock(&mural_lock); }
void mural_unlock_access(void) { pthread_mutex_unlock(&mural_lock); }

// =====================================================
//  Score, Dinheiro e TIMER
// =====================================================
void mural_add_score(void) {
    pthread_mutex_lock(&mural_lock);
    global_score++;
    pthread_mutex_unlock(&mural_lock);
}

int mural_get_score(void) {
    int s;
    pthread_mutex_lock(&mural_lock);
    s = global_score;
    pthread_mutex_unlock(&mural_lock);
    return s;
}

void mural_add_money(int amount) {
    pthread_mutex_lock(&mural_lock);
    global_money += amount;
    pthread_mutex_unlock(&mural_lock);
}

int mural_get_money(void) {
    int m;
    pthread_mutex_lock(&mural_lock);
    m = global_money;
    pthread_mutex_unlock(&mural_lock);
    return m;
}

void mural_setup_timer(int duration_seconds) {
    pthread_mutex_lock(&mural_lock);
    game_deadline = time(NULL) + duration_seconds;
    pthread_mutex_unlock(&mural_lock);
}

int mural_get_remaining_seconds(void) {
    pthread_mutex_lock(&mural_lock);
    if (game_deadline == 0) {
        pthread_mutex_unlock(&mural_lock);
        return 0;
    }
    time_t now = time(NULL);
    double diff = difftime(game_deadline, now);
    int ret = (int)diff;
    if (ret < 0) ret = 0;
    pthread_mutex_unlock(&mural_lock);
    return ret;
}