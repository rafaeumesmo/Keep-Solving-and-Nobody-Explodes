// https://github.com/rafaeumesmo/Keep-Solving-and-Nobody-Explodes.git

// Define para habilitar funções POSIX modernas (como nanosleep)
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

// ============================================================================
//  Variáveis configuráveis em tempo de execução (ajustadas pela dificuldade)
// ============================================================================

static int runtime_num_tedax = NUM_TEDAX;                     // Número de técnicos (Tedax)
static int runtime_num_benches = NUM_BENCHES;                 // Número de bancadas
static int runtime_module_gen_interval_ms = MODULE_GEN_INTERVAL_MS; // Intervalo entre geração de módulos
static int runtime_game_duration_sec = GAME_DURATION_SEC;     // Duração total da partida
static int runtime_module_timeout_sec = MODULE_TIMEOUT_SEC;   // Tempo limite por módulo

// Ajuste adicional para número de “PRESS” gerados em módulos tipo botão
static int runtime_max_press_count = 3;

// Variáveis das threads principais
static pthread_t gen_thread;
static pthread_t watcher_thread;

// Flag global indicando se o jogo está rodando
static volatile int running = 1;

// Semáforo das bancadas
static sem_t benches_sem;

// ============================================================================
//  Função para aplicar configurações baseadas na dificuldade escolhida
// ============================================================================
static void apply_difficulty_preset(int choice) {
    // 1 = Fácil | 2 = Médio | 3 = Difícil | 4 = Insano
    switch (choice) {
        case 1: // FACIL
            runtime_num_tedax = 1;
            runtime_num_benches = 1;
            runtime_module_timeout_sec = (int)(MODULE_TIMEOUT_SEC * 1.2); // mais tempo
            runtime_module_gen_interval_ms = MODULE_GEN_INTERVAL_MS * 2;  // spawn mais lento
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
            runtime_game_duration_sec = (int)(GAME_DURATION_SEC * 0.75);
            break;

        default:
            // fallback para o médio
            runtime_num_tedax = NUM_TEDAX;
            runtime_num_benches = NUM_BENCHES;
            runtime_module_timeout_sec = MODULE_TIMEOUT_SEC;
            runtime_module_gen_interval_ms = MODULE_GEN_INTERVAL_MS;
            runtime_max_press_count = 3;
            runtime_game_duration_sec = GAME_DURATION_SEC;
            break;
    }
}

// ============================================================================
//  Exibição do menu inicial no terminal
// ============================================================================
static int show_menu_and_get_choice(void) {
    int choice = 2; // padrão = médio

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
    if (!fgets(buf, sizeof(buf), stdin))
        return 2;

    // Ignora espaços iniciais
    char *p = buf;
    while (*p && isspace((unsigned char)*p)) p++;

    if (*p >= '1' && *p <= '4')
        choice = *p - '0';

    return choice;
}

// ============================================================================
//  Thread Geradora — cria módulos periodicamente
// ============================================================================
static void* generator_fn(void *arg) {
    (void)arg;
    int next_id = 1;

    srand((unsigned int)time(NULL)); // Semente aleatória

    while (running) {
        // Criação do módulo
        module_t *m = create_module(next_id++);

        if (!m) {
            log_event("[GEN] erro criar modulo");
        } else {
            // Substitui timeout pelo valor da dificuldade
            m->timeout_secs = runtime_module_timeout_sec;

            log_event("[GEN] M%d gerado (tipo %d) timeout=%ds",
                      m->id, m->type, m->timeout_secs);

            mural_push(m);
        }

        // Dorme pelo tempo configurado
        struct timespec ts;
        ts.tv_sec = runtime_module_gen_interval_ms / 1000;
        ts.tv_nsec = (runtime_module_gen_interval_ms % 1000) * 1000000;
        nanosleep(&ts, NULL);
    }

    return NULL;
}

// ============================================================================
//  Thread Watcher — monitora expiracao de módulos no mural
// ============================================================================
static void* watcher_fn(void *arg) {
    (void)arg;

    while (running) {
        module_t *cur = mural_peek_list();
        time_t now = time(NULL);

        while (cur) {
            int age = (int)(now - cur->created_at);
            int left = cur->timeout_secs - age;

            if (left <= 10 && left > 0) {
                log_event("ATENCAO: M%d critica (timeout %ds)", cur->id, left);
            }
            else if (left <= 0) {
                // Estourou o tempo → remove e re-enfileira
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

// ============================================================================
//  Função Principal
// ============================================================================
int main(void) {

    // 1) Exibe menu e aplica dificuldade
    int choice = show_menu_and_get_choice();
    apply_difficulty_preset(choice);

    printf("Dificuldade escolhida: %d -> TEDAX=%d BANCADAS=%d GEN_MS=%d TIMEOUT=%ds\n",
           choice, runtime_num_tedax, runtime_num_benches,
           runtime_module_gen_interval_ms, runtime_module_timeout_sec);

    printf("Pressione ENTER para iniciar o jogo...");
    fflush(stdout);
    getchar();

    // Inicializa mural, semáforos, UI e coordenador
    mural_init();

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

    // Cria pool de Tedax com número configurado
    tedax_pool_init(runtime_num_tedax, &benches_sem);

    // Cria threads paralelas
    running = 1;

    if (pthread_create(&gen_thread, NULL, generator_fn, NULL) != 0) {
        perror("pthread_create generator");
        running = 0;
    }

    if (pthread_create(&watcher_thread, NULL, watcher_fn, NULL) != 0) {
        perror("pthread_create watcher");
        running = 0;
    }

    // Loop principal de tempo do jogo
    int elapsed = 0;
    while (elapsed < runtime_game_duration_sec && running) {
        sleep(1);
        elapsed++;

        if (elapsed % 10 == 0)
            log_event("[TIMER] %ds elapsed", elapsed);
    }

    // Encerramento ordenado
    log_event("[SYSTEM] tempo esgotado / encerrando (elapsed=%d)", elapsed);
    running = 0;

    pthread_join(gen_thread, NULL);
    pthread_join(watcher_thread, NULL);

    coord_shutdown();

    tedax_pool_shutdown();
    tedax_pool_destroy();

    ui_stop();

    mural_destroy();
    sem_destroy(&benches_sem);

    // Placar final
    printf("Jogo finalizado. Obrigado!\n");
    printf(">>> PLACAR FINAL: %d Modulos Desarmados <<<\n", mural_get_score());

    return 0;
}
