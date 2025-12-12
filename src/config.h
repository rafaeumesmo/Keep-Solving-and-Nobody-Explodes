#ifndef CONFIG_H
#define CONFIG_H

// Gameplay / concorrência
#define NUM_TEDAX 3
#define NUM_BENCHES 2

// ALTERAÇÃO: Reduzido de 1200 para 500ms para spawnar mais rápido
#define MODULE_GEN_INTERVAL_MS 500   
#define GAME_DURATION_SEC 180         // tempo total do jogo (segundos)
#define MODULE_TIMEOUT_SEC 30         // timeout para módulos no mural (segundos)

// NOVO: Definições de economia
#define MOEDAS_INICIAL 0
#define MOEDAS_POR_MODULO 10

// UI / logs
#define LOG_LINES 256
#define UI_REFRESH_MS 100             // taxa de refresh da UI em ms

#endif // CONFIG_H