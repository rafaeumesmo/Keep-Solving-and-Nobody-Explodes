#define _POSIX_C_SOURCE 200809L
#include "tedax.h"
#include "mural.h"
#include "ui.h"         
#include "config.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <semaphore.h>
#include <pthread.h>
#include <ctype.h>

// pool
static tedax_t *pool = NULL;
static int pool_n = 0;
static volatile int pool_running = 0;

// benches bookkeeping
#ifdef NUM_BENCHES
static int num_benches = NUM_BENCHES;
#else
static int num_benches = 2;
#endif
static int *bench_busy = NULL;
static sem_t *external_benches_sem = NULL;

// mutexes
static pthread_mutex_t pool_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t bench_mutex = PTHREAD_MUTEX_INITIALIZER;

// helper: lowercase comparison
static int ci_equal(const char *a, const char *b) {
    if (!a || !b) return 0;
    while (*a && *b) {
        char ca = *a, cb = *b;
        if (ca >= 'A' && ca <= 'Z') ca = ca - 'A' + 'a';
        if (cb >= 'A' && cb <= 'Z') cb = cb - 'A' + 'a';
        if (ca != cb) return 0;
        a++; b++;
    }
    return *a == '\0' && *b == '\0';
}

// acquire a free bench index (returns index or -1)
static int bench_acquire_index_blocking(void) {
    if (external_benches_sem) sem_wait(external_benches_sem);

    int idx = -1;
    while (1) {
        pthread_mutex_lock(&bench_mutex);
        for (int i = 0; i < num_benches; ++i) {
            if (!bench_busy[i]) {
                bench_busy[i] = 1;
                idx = i;
                break;
            }
        }
        pthread_mutex_unlock(&bench_mutex);
        if (idx >= 0) return idx;
        
        struct timespec ts;
        ts.tv_sec = 0;
        ts.tv_nsec = 100 * 1000000; // 100ms
        nanosleep(&ts, NULL);
    }
}

static int bench_try_acquire_index(void) {
    pthread_mutex_lock(&bench_mutex);
    int idx = -1;
    for (int i = 0; i < num_benches; ++i) {
        if (!bench_busy[i]) {
            bench_busy[i] = 1;
            idx = i;
            break;
        }
    }
    pthread_mutex_unlock(&bench_mutex);
    return idx;
}

static void bench_release_index(int idx) {
    if (idx < 0 || idx >= num_benches) return;
    pthread_mutex_lock(&bench_mutex);
    bench_busy[idx] = 0;
    pthread_mutex_unlock(&bench_mutex);
    if (external_benches_sem) sem_post(external_benches_sem);
}

static void* tedax_thread_fn(void *arg) {
    tedax_t *self = (tedax_t*)arg;

    while (pool_running) {
        pthread_mutex_lock(&self->lock);
        while (!self->current && pool_running) {
            pthread_cond_wait(&self->cond, &self->lock);
        }
        if (!pool_running && !self->current) {
            pthread_mutex_unlock(&self->lock);
            break;
        }

        module_t *m = self->current;
        self->busy = 1;
        self->start_time = time(NULL);
        self->remaining = m->time_required;
        int assigned_bench = self->bench_id; 

        pthread_mutex_unlock(&self->lock);

        if (assigned_bench < 0) {
            log_event("[T%d] aguardando bancada para M%d...", self->id, m->id);
            assigned_bench = bench_acquire_index_blocking();
            pthread_mutex_lock(&self->lock);
            self->bench_id = assigned_bench;
            pthread_mutex_unlock(&self->lock);
            log_event("[T%d] bancada %d ocupada para M%d", self->id, assigned_bench, m->id);
        } else {
            log_event("[T%d] bancada %d confirmada (pre-assign) M%d", self->id, assigned_bench, m->id);
        }

        int elapsed = 0;
        while (pool_running && elapsed < m->time_required) {
            sleep(1);
            elapsed++;
            time_t now = time(NULL);
            if (now > (m->created_at + m->timeout_secs)) {
                log_event("[T%d] 💥 M%d EXPLODIU na mao! (Timeout)", self->id, m->id);
                self->remaining = 0;
                m->instruction[0] = '\0'; // Garante falha
                break; 
            }
        }

        int success = 0;
        
        // Lógica de Sucesso (Igual à anterior: Manual vs Auto)
        if (m->instruction[0] != '\0') {
            if (ci_equal(m->instruction, m->solution)) success = 1;
            else success = 0;
        } 
        else {
            int chance = rand() % 100;
            if (chance < 60) success = 1;
            else { success = 0; log_event("[T%d] IA falhou no desarmamento automatico.", self->id); }
        }

        bench_release_index(assigned_bench);

        if (success) {
            log_event("[T%d] ✔ M%d DESARMADO (+%d Gold)", self->id, m->id, MOEDAS_POR_MODULO);
            mural_add_score();
            mural_add_money(MOEDAS_POR_MODULO);
            
            // --- ALTERAÇÃO AQUI: Move para resolvidos em vez de free() ---
            mural_add_to_resolved(m);
            // -------------------------------------------------------------
        } else {
            log_event("[T%d] ✖ M%d FALHOU — re-enfileirado", self->id, m->id);
            m->instruction[0] = '\0'; 
            // Ao re-enfileirar, reduzir o tempo restante do módulo (penalidade)
            // Calculamos uma redução baseada no tempo gasto (elapsed)
            int reduction = elapsed; // reduzir em segundos iguais ao tempo gasto
            if (reduction <= 0) reduction = 1;
            int new_timeout = m->timeout_secs - reduction;
            if (new_timeout < 1) new_timeout = 1;
            m->timeout_secs = new_timeout;
            m->created_at = time(NULL); // reinicia criação para usar novo timeout
            mural_requeue(m);
        }

        pthread_mutex_lock(&self->lock);
        self->current = NULL;
        self->bench_id = -1;
        self->busy = 0;
        self->start_time = 0;
        self->remaining = 0;
        pthread_mutex_unlock(&self->lock);
    }

    return NULL;
}

void tedax_pool_init(int n, int benches_count, sem_t *benches_sem) {
    if (n <= 0) return;
    pthread_mutex_lock(&pool_mutex);

    external_benches_sem = benches_sem;
    pool_n = n;
    pool_running = 1;

    if (benches_count > 0) num_benches = benches_count;
    else {
#ifdef NUM_BENCHES
        num_benches = NUM_BENCHES;
#else
        num_benches = 2;
#endif
    }
    bench_busy = calloc(num_benches, sizeof(int));

    pool = calloc(pool_n, sizeof(tedax_t));
    for (int i = 0; i < pool_n; ++i) {
        pool[i].id = i;
        pool[i].busy = 0;
        pool[i].current = NULL;
        pool[i].bench_id = -1;
        pool[i].start_time = 0;
        pool[i].remaining = 0;
        pthread_mutex_init(&pool[i].lock, NULL);
        pthread_cond_init(&pool[i].cond, NULL);
        pthread_create(&pool[i].thr, NULL, tedax_thread_fn, &pool[i]);
    }

    pthread_mutex_unlock(&pool_mutex);
    log_event("[SYSTEM] Tedax pool iniciado: %d unidades, %d bancadas", pool_n, num_benches);
}

int tedax_bench_count(void) {
    return num_benches;
}

void tedax_pool_shutdown(void) {
    pool_running = 0;
    if (!pool) return;
    for (int i = 0; i < pool_n; ++i) {
        pthread_mutex_lock(&pool[i].lock);
        pthread_cond_signal(&pool[i].cond);
        pthread_mutex_unlock(&pool[i].lock);
    }
    if (external_benches_sem) {
        for (int i=0;i<num_benches;i++) sem_post(external_benches_sem);
    }
}

void tedax_pool_destroy(void) {
    if (!pool) return;
    for (int i = 0; i < pool_n; ++i) {
        pthread_join(pool[i].thr, NULL);
        pthread_mutex_destroy(&pool[i].lock);
        pthread_cond_destroy(&pool[i].cond);
    }
    free(pool);
    pool = NULL;
    pool_n = 0;

    if (bench_busy) free(bench_busy);
    bench_busy = NULL;
    log_event("[SYSTEM] Tedax pool destruido");
}

int tedax_assign_module(int id, module_t *m) {
    if (!pool || id < 0 || id >= pool_n || !m) return -1;
    tedax_t *t = &pool[id];

    pthread_mutex_lock(&t->lock);
    if (t->busy || t->current) {
        pthread_mutex_unlock(&t->lock);
        return -1;
    }
    t->current = m;
    t->bench_id = -1; 
    t->start_time = time(NULL);
    t->remaining = m->time_required;
    t->busy = 1;
    pthread_cond_signal(&t->cond);
    pthread_mutex_unlock(&t->lock);

    log_event("[ASSIGN] M%d → T%d", m->id, id);
    return 0;
}

int tedax_request_auto(module_t *m) {
    if (!m || !pool) return -1;

    int chosen = -1;
    for (int i = 0; i < pool_n; ++i) {
        pthread_mutex_lock(&pool[i].lock);
        int busy = pool[i].busy || (pool[i].current != NULL);
        pthread_mutex_unlock(&pool[i].lock);
        if (!busy) { chosen = i; break; }
    }
    if (chosen < 0) return -1;

    int bidx = bench_try_acquire_index();
    if (bidx < 0) {
        return -1;
    }

    pthread_mutex_lock(&pool[chosen].lock);
    pool[chosen].current = m;
    pool[chosen].bench_id = bidx;
    pool[chosen].start_time = time(NULL);
    pool[chosen].remaining = m->time_required;
    pool[chosen].busy = 1;
    pthread_cond_signal(&pool[chosen].cond);
    pthread_mutex_unlock(&pool[chosen].lock);

    log_event("[AUTO] M%d -> T%d B%d", m->id, chosen, bidx);
    return chosen;
}

int tedax_request_manual(module_t *m, int tedax_id, int bench_id, int presses) {
    (void)presses;

    if (!m || !pool) return 0;
    if (tedax_id < 0 || tedax_id >= pool_n) return 0;
    if (bench_id < 0 || bench_id >= num_benches) return 0;

    pthread_mutex_lock(&pool[tedax_id].lock);
    if (pool[tedax_id].busy || pool[tedax_id].current) {
        pthread_mutex_unlock(&pool[tedax_id].lock);
        return 0;
    }
    pthread_mutex_unlock(&pool[tedax_id].lock);

    int ok = 0;
    pthread_mutex_lock(&bench_mutex);
    if (!bench_busy[bench_id]) {
        bench_busy[bench_id] = 1;
        ok = 1;
    }
    pthread_mutex_unlock(&bench_mutex);
    if (!ok) return 0;

    pthread_mutex_lock(&pool[tedax_id].lock);
    pool[tedax_id].current = m;
    pool[tedax_id].bench_id = bench_id;
    pool[tedax_id].start_time = time(NULL);
    pool[tedax_id].remaining = m->time_required;
    pool[tedax_id].busy = 1;
    pthread_cond_signal(&pool[tedax_id].cond);
    pthread_mutex_unlock(&pool[tedax_id].lock);

    log_event("[MANUAL] M%d -> T%d B%d (manual)", m->id, tedax_id, bench_id);
    return 1;
}

tedax_t* tedax_get(int id) {
    if (!pool || id < 0 || id >= pool_n) return NULL;
    return &pool[id];
}

int tedax_count(void) {
    return pool_n;
}