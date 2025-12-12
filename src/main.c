// https://github.com/rafaeumesmo/Keep-Solving-and-Nobody-Explodes.git

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

// Configurações globais
static int runtime_num_tedax = NUM_TEDAX;
static int runtime_num_benches = NUM_BENCHES;
static int runtime_module_gen_interval_ms = MODULE_GEN_INTERVAL_MS;
static int runtime_game_duration_sec = GAME_DURATION_SEC;
static int runtime_module_timeout_sec = MODULE_TIMEOUT_SEC;
static int runtime_max_press_count = 3;

static pthread_t gen_thread;
static pthread_t watcher_thread;
static volatile int running = 1;
static sem_t benches_sem;

static void apply_difficulty_preset(int choice) {
    switch (choice) {
        case 1: // FACIL
            runtime_num_tedax = 1;
            runtime_num_benches = 1;
            runtime_module_timeout_sec = (int)(MODULE_TIMEOUT_SEC * 1.2);
            runtime_module_gen_interval_ms = MODULE_GEN_INTERVAL_MS * 2;
            runtime_max_press_count = 2;
            break;
        case 2: // MEDIO
            runtime_num_tedax = 2;
            runtime_num_benches = 2;
            runtime_module_timeout_sec = MODULE_TIMEOUT_SEC;
            runtime_module_gen_interval_ms = MODULE_GEN_INTERVAL_MS;
            runtime_max_press_count = 3;
            break;
        case 3: // DIFICIL
            runtime_num_tedax = 3;
            runtime_num_benches = 3;
            runtime_module_timeout_sec = (int)(MODULE_TIMEOUT_SEC * 0.8);
            runtime_module_gen_interval_ms = (int)(MODULE_GEN_INTERVAL_MS * 0.8);
            runtime_max_press_count = 4;
            break;
        case 4: // INSANO
            runtime_num_tedax = 3;
            runtime_num_benches = 2;
            runtime_module_timeout_sec = (int)(MODULE_TIMEOUT_SEC * 0.6);
            runtime_module_gen_interval_ms = (int)(MODULE_GEN_INTERVAL_MS * 0.6);
            runtime_max_press_count = 5;
            runtime_game_duration_sec = (int)(GAME_DURATION_SEC * 0.75); // Tempo menor no insano
            break;
        default:
            runtime_num_tedax = NUM_TEDAX;
            runtime_num_benches = NUM_BENCHES;
            runtime_module_timeout_sec = MODULE_TIMEOUT_SEC;
            runtime_module_gen_interval_ms = MODULE_GEN_INTERVAL_MS;
            runtime_max_press_count = 3;
            break;
    }
}

static int show_menu_and_get_choice(void) {
    int choice = 2;
    printf("\n=====================================\n");
    printf("   Keep Solving and Nobody Explodes  \n");
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
    char *p = buf;
    while (*p && isspace((unsigned char)*p)) p++;
    if (*p >= '1' && *p <= '4') choice = *p - '0';
    return choice;
}

static void* generator_fn(void *arg) {
    (void)arg;
    int next_id = 1;
    srand((unsigned int)time(NULL));

    while (running) {
        module_t *m = create_module(next_id++);
        if (m) {
            m->timeout_secs = runtime_module_timeout_sec;
            log_event("[GEN] M%d gerado (tipo %d) timeout=%ds", m->id, m->type, m->timeout_secs);
            mural_push(m);
        }
        struct timespec ts;
        ts.tv_sec = runtime_module_gen_interval_ms / 1000;
        ts.tv_nsec = (runtime_module_gen_interval_ms % 1000) * 1000000;
        nanosleep(&ts, NULL);
    }
    return NULL;
}

static void* watcher_fn(void *arg) {
    (void)arg;
    while (running) {
        module_t *cur = mural_peek_list();
        time_t now = time(NULL);
        while (cur) {
            int age = (int)(now - cur->created_at);
            int left = cur->timeout_secs - age;
            if (left <= 10 && left > 0) {
                // log_event("ATENCAO: M%d critica (timeout %ds)", cur->id, left);
            }
            else if (left <= 0) {
                module_t *expired = mural_pop_by_id(cur->id);
                if (expired) {
                    log_event("[WATCHER] M%d TIMEOUT — requeue", expired->id);
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
    int choice = show_menu_and_get_choice();
    apply_difficulty_preset(choice);

    printf("Dificuldade: %d -> TEDAX=%d BANCADAS=%d GEN_MS=%d GAME_TIME=%ds\n",
           choice, runtime_num_tedax, runtime_num_benches,
           runtime_module_gen_interval_ms, runtime_game_duration_sec);

    printf("Pressione ENTER para iniciar o jogo...");
    fflush(stdout);
    getchar();

    // 1. Inicializa o Mural
    mural_init();
    
    // 2. CONFIGURA O TIMER GLOBAL DO JOGO (NOVO)
    mural_setup_timer(runtime_game_duration_sec);

    if (sem_init(&benches_sem, 0, (unsigned int)runtime_num_benches) != 0) {
        perror("sem_init");
        return 1;
    }

    ui_start();

    if (coord_start() != 0) {
        fprintf(stderr, "Erro ao iniciar coordenador\n");
        ui_stop();
        sem_destroy(&benches_sem);
        return 1;
    }

    tedax_pool_init(runtime_num_tedax, &benches_sem);

    running = 1;
    if (pthread_create(&gen_thread, NULL, generator_fn, NULL) != 0) {
        perror("pthread_create generator");
        running = 0;
    }
    if (pthread_create(&watcher_thread, NULL, watcher_fn, NULL) != 0) {
        perror("pthread_create watcher");
        running = 0;
    }

    // Loop principal: Verifica se o tempo acabou consultando o mural
    while (running) {
        sleep(1);
        
        int remaining = mural_get_remaining_seconds();
        
        // Se o tempo acabou (0 segundos), encerra o jogo
        if (remaining <= 0) {
            log_event("[SYSTEM] TEMPO ESGOTADO! FIM DE JOGO.");
            sleep(2); // Pequena pausa para ler o log
            running = 0;
            break;
        }

        // Log a cada 30s só para debug
        if (remaining % 30 == 0)
            log_event("[TIMER] Restam %d segundos...", remaining);
    }

    // Encerramento
    pthread_join(gen_thread, NULL);
    pthread_join(watcher_thread, NULL);
    coord_shutdown();
    tedax_pool_shutdown();
    tedax_pool_destroy();
    ui_stop();
    mural_destroy();
    sem_destroy(&benches_sem);

    printf("\n\n============================================\n");
    printf("           JOGO FINALIZADO!                 \n");
    printf("============================================\n");
    printf(">>> SCORE FINAL: %d Modulos Desarmados \n", mural_get_score());
    printf(">>> GOLD FINAL:  %d Moedas \n", mural_get_money());
    printf("============================================\n");

    return 0;
}