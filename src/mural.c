#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <time.h>

#include "mural.h"
#include "ui.h"

static module_t *mural_head = NULL;
static module_t *mural_tail = NULL;

pthread_mutex_t mural_lock = PTHREAD_MUTEX_INITIALIZER;
static int mural_size = 0;

// =====================================================
//  Criação de Módulos
// =====================================================

module_t* create_module(int id) {
    module_t *m = calloc(1, sizeof(module_t));
    if (!m) return NULL;

    m->id = id;
    m->type = rand() % 3;             // FIOS / BOTAO / SENHAS
    m->time_required = 5 + rand() % 6;
    m->created_at = time(NULL);
    m->timeout_secs = 20 + rand() % 10;
    m->instruction[0] = '\0';

    // Gera solução dependendo do tipo
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
//  Inserção na fila
// =====================================================

void mural_push(module_t *m) {
    pthread_mutex_lock(&mural_lock);

    if (!mural_head) {
        mural_head = mural_tail = m;
    } else {
        mural_tail->next = m;
        mural_tail = m;
    }

    mural_size++;
    log_event("[MURAL] M%d adicionado", m->id);

    pthread_mutex_unlock(&mural_lock);
}

// =====================================================
//  Remover o primeiro elemento
// =====================================================

module_t* mural_pop_front(void) {
    pthread_mutex_lock(&mural_lock);

    if (!mural_head) {
        pthread_mutex_unlock(&mural_lock);
        return NULL;
    }

    module_t *m = mural_head;
    mural_head = mural_head->next;

    if (!mural_head) mural_tail = NULL;

    m->next = NULL;
    mural_size--;

    pthread_mutex_unlock(&mural_lock);
    return m;
}

// Alias para pop_front
module_t* mural_pop(void) {
    return mural_pop_front();
}

// =====================================================
//  Remover módulo por ID
// =====================================================

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

// =====================================================
//  Recolocar módulo no fim da fila
// =====================================================

void mural_requeue(module_t *m) {
    if (!m) return;

    pthread_mutex_lock(&mural_lock);

    m->next = NULL;

    if (!mural_head) {
        mural_head = mural_tail = m;
    } else {
        mural_tail->next = m;
        mural_tail = m;
    }

    mural_size++;
    log_event("[MURAL] M%d re-enfileirado", m->id);

    pthread_mutex_unlock(&mural_lock);
}

// =====================================================
//  Peek da lista inteira
// =====================================================

module_t* mural_peek_list(void) {
    return mural_head;
}

int mural_count(void) {
    return mural_size;
}

// =====================================================
//  Buscar módulo pelo tipo (não depende do TEDAX)
// =====================================================

module_t* mural_find_by_tedax_type(int tedax_id, char type_char) {
    (void)tedax_id; // TEDAX não influencia módulo

    pthread_mutex_lock(&mural_lock);

    module_t *cur = mural_head;

    while (cur) {

        char cur_type_char =
            (cur->type == MOD_FIOS)   ? 'F' :
            (cur->type == MOD_BOTAO)  ? 'B' :
            (cur->type == MOD_SENHAS) ? 'S' : '?';

        if (cur_type_char == type_char) {
            pthread_mutex_unlock(&mural_lock);
            return cur;
        }

        cur = cur->next;
    }

    pthread_mutex_unlock(&mural_lock);
    return NULL;
}

// =====================================================
//  Inicializar / liberar mural
// =====================================================

void mural_init(void) {
    pthread_mutex_lock(&mural_lock);
    mural_head = mural_tail = NULL;
    mural_size = 0;
    pthread_mutex_unlock(&mural_lock);
}

void mural_destroy(void) {
    pthread_mutex_lock(&mural_lock);
    module_t *cur = mural_head;
    while (cur) {
        module_t *n = cur->next;
        free(cur);
        cur = n;
    }
    mural_head = mural_tail = NULL;
    mural_size = 0;
    pthread_mutex_unlock(&mural_lock);
}
