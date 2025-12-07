#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <pthread.h>
#include <semaphore.h>
#include <time.h>
#include <ctype.h>
#include <string.h>

#include "config.h"
#include "mural.h"
#include "tedax.h"
#include "ui.h"
#include "coordinator.h"

// Runtime-configuráveis (valores padrão vindos de config.h)
static int runtime_num_tedax = NUM_TEDAX;
static int runtime_num_benches = NUM_BENCHES;
static int runtime_module_gen_interval_ms = MODULE_GEN_INTERVAL_MS;
static int runtime_game_duration_sec = GAME_DURATION_SEC;
static int runtime_module_timeout_sec = MODULE_TIMEOUT_SEC;

// Additional game tuning
static int runtime_max_press_count = 3; // controls "pp" length when generating modules

static pthread_t gen_thread;
static pthread_t watcher_thread;
static volatile int running = 1;
static sem_t benches_sem;

static void apply_difficulty_preset(int choice) {
    // choice: 1 = FACIL, 2 = MEDIO, 3 = DIFICIL, 4 = INSANO
    switch (choice) {
        case 1: // FACIL
            runtime_num_tedax = 1;
            runtime_num_benches = 1;
            runtime_module_timeout_sec = (int)(MODULE_TIMEOUT_SEC * 1.2);
            runtime_module_gen_interval_ms = MODULE_GEN_INTERVAL_MS * 2; // slower spawn
            runtime_max_press_count = 2;
            runtime_game_duration_sec = GAME_DURATION_SEC;
            break;
        case 2: // MEDIO (padrão)
            runtime_num_tedax = 2;
            runtime_num_benches = 2;
            runtime_module_timeout_sec = MODULE_TIMEOUT_SEC;
            runtime_module_gen_interval_ms = MODULE_GEN_INTERVAL_MS;
            runtime_max_press_count = 3;
            runtime_game_duration_sec = GAME_DURATION_SEC;
            break;
        case 3: // DIFICIL
            runtime_num_tedax = 3;
            runtime_num_benches = 3;
            runtime_module_timeout_sec = (int)(MODULE_TIMEOUT_SEC * 0.8);
            runtime_module_gen_interval_ms = (int)(MODULE_GEN_INTERVAL_MS * 0.8);
            runtime_max_press_count = 4;
            runtime_game_duration_sec = GAME_DURATION_SEC;
            break;
        case 4: // INSANO
            runtime_num_tedax = 3;
            runtime_num_benches = 2;
            runtime_module_timeout_sec = (int)(MODULE_TIMEOUT_SEC * 0.6);
            runtime_module_gen_interval_ms = (int)(MODULE_GEN_INTERVAL_MS * 0.6);
            runtime_max_press_count = 5;
            // reduce total game time for insanity
            runtime_game_duration_sec = (int)(GAME_DURATION_SEC * 0.75);
            break;
        default:
            // fallback to medium
            runtime_num_tedax = NUM_TEDAX;
            runtime_num_benches = NUM_BENCHES;
            runtime_module_timeout_sec = MODULE_TIMEOUT_SEC;
            runtime_module_gen_interval_ms = MODULE_GEN_INTERVAL_MS;
            runtime_max_press_count = 3;
            runtime_game_duration_sec = GAME_DURATION_SEC;
            break;
    }
}

static int show_menu_and_get_choice(void) {
    int choice = 2;
    printf("\n=====================================\n");
    printf("   Keep Solving and Nobody Explodes         \n");
    printf("=====================================\n\n");
    printf("Escolha a dificuldade (tecle o numero e Enter):\n\n");
    printf("  [1] FACIL   (1 Tedax, 1 bancada, spawn lento)\n");
    printf("  [2] MEDIO   (2 Tedax, 2 bancadas) (padrao)\n");
    printf("  [3] DIFICIL (3 Tedax, 3 bancadas, spawn rapido)\n");
    printf("  [4] INSANO  (3 Tedax, 2 bancadas, spawn muito rapido)\n\n");
    printf("Escolha: ");
    fflush(stdout);

    char buf[16];
    if (!fgets(buf, sizeof(buf), stdin)) return 2;
    // skip leading whitespace
    char *p = buf;
    while (*p && isspace((unsigned char)*p)) p++;
    if (*p >= '1' && *p <= '4') choice = *p - '0';
    return choice;
}

// Generator: creates modules and sets their timeout based on the runtime value.
// Also generate the "code" (e.g., 3f2pp) inside create_module or we can augment it here.
// We will set m->timeout_secs to runtime_module_timeout_sec after creation.
static void* generator_fn(void *arg) {
    (void)arg;
    int next_id = 1;
    // seed randomness for any random choices inside create_module if used
    srand((unsigned int)time(NULL));
    while (running) {
        module_t *m = create_module(next_id++);
        if (!m) {
            log_event("[GEN] erro criar modulo");
        } else {
            // override module timeout with difficulty-specific runtime value
            m->timeout_secs = runtime_module_timeout_sec;

            // optionally, if create_module supports a 'press count' we might want to adjust,
            // but we'll assume create_module uses id-based rules; we could also embed a dynamic press count
            // by using the m->id to influence complexity. For now we log for debug.
            log_event("[GEN] M%d gerado (tipo %d) timeout=%ds", m->id, m->type, m->timeout_secs);

            mural_push(m);
        }

        // sleep according to runtime interval
        struct timespec ts;
        ts.tv_sec = runtime_module_gen_interval_ms / 1000;
        ts.tv_nsec = (runtime_module_gen_interval_ms % 1000) * 1000000;
        nanosleep(&ts, NULL);
    }
    return NULL;
}

// watcher: check for module timeouts and log urgent warnings
static void* watcher_fn(void *arg) {
    (void)arg;
    while (running) {
        module_t *cur = mural_peek_list();
        time_t now = time(NULL);
        // iterate through mural list; mural_peek_list returns head pointer (do not modify)
        while (cur) {
            int age = (int)(now - cur->created_at);
            int left = cur->timeout_secs - age;
            if (left <= 10 && left > 0) {
                log_event("ATENCAO: M%d critica (timeout %ds)", cur->id, left);
            } else if (left <= 0) {
                // timeout reached: pop by id and requeue
                module_t *expired = mural_pop_by_id(cur->id);
                if (expired) {
                    log_event("[WATCHER] M%d TIMEOUT — requeue", expired->id);
                    // optionally penalize here (e.g., decrease something); for now, just requeue
                    mural_requeue(expired);
                }
            }
            cur = cur->next;
        }
        sleep(1);
    }
    return NULL;
}

int main(void) {
    // 1) menu
    int choice = show_menu_and_get_choice();
    apply_difficulty_preset(choice);
    printf("Dificuldade escolhida: %d -> TEDAX=%d BANCADAS=%d GEN_MS=%d TIMEOUT=%ds\n",
           choice, runtime_num_tedax, runtime_num_benches,
           runtime_module_gen_interval_ms, runtime_module_timeout_sec);
    printf("Pressione ENTER para iniciar o jogo...");
    fflush(stdout);
    getchar(); // wait enter

    // init core systems
    mural_init();

    // init benches semaphore with runtime count
    if (sem_init(&benches_sem, 0, (unsigned int)runtime_num_benches) != 0) {
        perror("sem_init");
        return 1;
    }

    // start UI (UI will draw and enqueue commands to coordinator)
    ui_start();

    // start coordinator (must be started before tedax to accept enqueued commands)
    if (coord_start() != 0) {
        fprintf(stderr, "Erro ao iniciar coordenador\n");
        // cleanup ui and exit
        ui_stop();
        sem_destroy(&benches_sem);
        return 1;
    }

    // start tedax pool with runtime count
    tedax_pool_init(runtime_num_tedax, &benches_sem);

    // start generator and watcher threads
    running = 1;
    if (pthread_create(&gen_thread, NULL, generator_fn, NULL) != 0) {
        perror("pthread_create generator");
        running = 0;
    }
    if (pthread_create(&watcher_thread, NULL, watcher_fn, NULL) != 0) {
        perror("pthread_create watcher");
        running = 0;
    }

    // main loop: wait for game duration
    int elapsed = 0;
    while (elapsed < runtime_game_duration_sec && running) {
        sleep(1);
        elapsed++;
        if (elapsed % 10 == 0) log_event("[TIMER] %ds elapsed", elapsed);
    }

    // shutdown sequence
    log_event("[SYSTEM] tempo esgotado / encerrando (elapsed=%d)", elapsed);
    running = 0;

    // join threads
    pthread_join(gen_thread, NULL);
    pthread_join(watcher_thread, NULL);

    // shutdown coordinator
    coord_shutdown();

    // stop tedax and destroy
    tedax_pool_shutdown();
    tedax_pool_destroy();

    // stop UI
    ui_stop();

    // cleanup mural and sem
    mural_destroy();
    sem_destroy(&benches_sem);

    printf("Jogo finalizado. Obrigado!\n");
    return 0;
}
