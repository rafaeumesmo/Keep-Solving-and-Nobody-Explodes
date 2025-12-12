#ifndef CONFIG_H
#define CONFIG_H

// Gameplay / concorrência
#define NUM_TEDAX 3
#define NUM_BENCHES 2

#define MODULE_GEN_INTERVAL_MS 500   // Spawn rápido
#define GAME_DURATION_SEC 180        // Duração total do jogo (segundos)
#define MODULE_TIMEOUT_SEC 30        // timeout para módulos no mural

// Economia
#define MOEDAS_INICIAL 0
#define MOEDAS_POR_MODULO 10

// UI / logs
#define LOG_LINES 256
#define UI_REFRESH_MS 100            // taxa de refresh da UI em ms

#endif // CONFIG_H