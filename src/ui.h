#ifndef UI_H
#define UI_H

// Inicia/Para a thread da UI
void ui_start(void);
void ui_stop(void);

// Verifica se a UI ainda está a correr (retorna 0 se o jogador carregou em Q)
int is_ui_active(void);

// Menus (Bloqueantes)
void show_start_screen(void);
int show_main_menu_ncurses(void);
int show_difficulty_menu_ncurses(void);

// Logging
void log_event(const char *fmt, ...);

#endif // UI_H