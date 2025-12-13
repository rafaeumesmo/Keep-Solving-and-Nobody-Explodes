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

static pthread_t gen_thread;
static pthread_t watcher_thread;
static volatile int running = 1;
static sem_t benches_sem;

static void apply_difficulty_preset(int choice) {
    switch (choice) {
        case 1: // FACIL
            runtime_num_tedax = 1; 
            runtime_num_benches = 1;
            runtime_module_timeout_sec = (int)(MODULE_TIMEOUT_SEC * 1.5);
            runtime_module_gen_interval_ms = MODULE_GEN_INTERVAL_MS * 2;
            break;
        case 2: // MEDIO
            runtime_num_tedax = 2; 
            runtime_num_benches = 2;
            runtime_module_timeout_sec = MODULE_TIMEOUT_SEC;
            runtime_module_gen_interval_ms = MODULE_GEN_INTERVAL_MS;
            break;
        case 3: // DIFICIL
            runtime_num_tedax = 3; 
            runtime_num_benches = 3;
            runtime_module_timeout_sec = (int)(MODULE_TIMEOUT_SEC * 0.8);
            runtime_module_gen_interval_ms = (int)(MODULE_GEN_INTERVAL_MS * 0.8);
            break;
        case 4: // INSANO
            runtime_num_tedax = 3; 
            runtime_num_benches = 2;
            runtime_module_timeout_sec = (int)(MODULE_TIMEOUT_SEC * 0.6);
            runtime_module_gen_interval_ms = (int)(MODULE_GEN_INTERVAL_MS * 0.6);
            runtime_game_duration_sec = (int)(GAME_DURATION_SEC * 0.75);
            break;
        default: break;
    }
}

static void* generator_fn(void *arg) {
    (void)arg; int next_id = 1; srand((unsigned int)time(NULL));
    while (running) {
        module_t *m = create_module(next_id++);
        if (m) {
            m->timeout_secs = runtime_module_timeout_sec;
            log_event("[GEN] M%d gerado (tipo %d)", m->id, m->type);
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
            if (left <= 0) {
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
    show_start_screen();

    // Loop Principal da Aplicação
    while (1) {
        // --- MENU ---
        int diff_choice = 2;
        int menu_opt = show_main_menu_ncurses();
        
        if (menu_opt == 2) { // SAIR
            break; 
        } else if (menu_opt == 0) { // JOGAR
            diff_choice = show_difficulty_menu_ncurses();
        } else {
            continue; // Opções não implementadas
        }

        // --- PREPARAÇÃO DO JOGO ---
        apply_difficulty_preset(diff_choice);
        mural_init();
        mural_setup_timer(runtime_game_duration_sec);
        if (sem_init(&benches_sem, 0, (unsigned int)runtime_num_benches) != 0) return 1;

        running = 1; // Reset da flag global
        ui_start();
        if (coord_start() != 0) { ui_stop(); sem_destroy(&benches_sem); return 1; }
        tedax_pool_init(runtime_num_tedax, &benches_sem);
        pthread_create(&gen_thread, NULL, generator_fn, NULL);
        pthread_create(&watcher_thread, NULL, watcher_fn, NULL);

        // --- LOOP DO JOGO ---
        while (running) {
            sleep(1);
            
            // 1. Verifica tempo
            int rem = mural_get_remaining_seconds();
            if (rem <= 0) {
                log_event("[SYSTEM] TEMPO ESGOTADO!");
                sleep(2);
                running = 0;
            }

            // 2. Verifica se o utilizador pressionou Q (UI fechou)
            if (!is_ui_active()) {
                running = 0;
            }
        }

        // --- CLEANUP ---
        pthread_join(gen_thread, NULL);
        pthread_join(watcher_thread, NULL);
        coord_shutdown();
        tedax_pool_shutdown();
        tedax_pool_destroy();
        ui_stop(); 
        mural_destroy();
        sem_destroy(&benches_sem);
    }

    printf("Obrigado por jogar KEEP SOLVING!\n");
    return 0;
}