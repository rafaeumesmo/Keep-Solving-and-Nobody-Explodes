#define _POSIX_C_SOURCE 200809L
#include "ui.h"
#include "mural.h"
#include "tedax.h"
#include "config.h"
#include "coordinator.h" 

#include <ncurses.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h> 
#include <time.h>
#include <unistd.h>

static pthread_t ui_thread;
static volatile int ui_running = 0;

// Estados da Interface (Seleção por setas)
enum { MODE_NORMAL=0, MODE_SEL_MOD, MODE_SEL_TEDAX, MODE_SEL_BENCH };
static int ui_mode = MODE_NORMAL;
static int sel_idx = 0; // Índice visual do item selecionado

// IDs capturados durante o fluxo de seleção
static int selected_mod_id = -1;
static int selected_tedax_id = -1;
static int selected_bench_id = -1;

// windows
static WINDOW *w_header, *w_mural, *w_tedax, *w_bench, *w_log, *w_cmd;

// log buffer
static char *logbuf[LOG_LINES];
static int log_pos = -1;
static pthread_mutex_t log_lock = PTHREAD_MUTEX_INITIALIZER;

void log_event(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    char tmp[256];
    vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);

    pthread_mutex_lock(&log_lock);
    log_pos = (log_pos + 1) % LOG_LINES;
    if (logbuf[log_pos]) free(logbuf[log_pos]);
    // prefix with time
    char *entry = malloc(320);
    time_t t = time(NULL);
    struct tm tm = *localtime(&t);
    snprintf(entry, 320, "[%02d:%02d:%02d] %s", tm.tm_hour, tm.tm_min, tm.tm_sec, tmp);
    logbuf[log_pos] = entry;
    pthread_mutex_unlock(&log_lock);
}

// helper colors
enum { CP_DEFAULT=1, CP_TITLE, CP_HEADER, CP_OK, CP_WARN, CP_ERR, CP_ACCENT, CP_SELECT };

static void draw_border_title(WINDOW *w, const char *title) {
    werase(w);
    box(w, 0, 0);
    wattron(w, A_BOLD | COLOR_PAIR(CP_TITLE));
    mvwprintw(w, 0, 2, " %s ", title);
    wattroff(w, A_BOLD | COLOR_PAIR(CP_TITLE));
}

// produce nice time string from seconds
static void seconds_to_mmss(int s, char *buf, int len) {
    int m = s / 60;
    int sec = s % 60;
    snprintf(buf, len, "%02d:%02d", m, sec);
}

static void draw_header(int cols) {
    (void)cols; 
    werase(w_header);
    wattron(w_header, A_BOLD | COLOR_PAIR(CP_HEADER));
    // Mostra Score e Dinheiro (assumindo que mural_get_money existe no mural.h atualizado)
    mvwprintw(w_header, 0, 2, " LAST SPROUT - BOMB PANEL | SCORE: %d | GOLD: %d ", 
              mural_get_score(), mural_get_money());
    wattroff(w_header, A_BOLD | COLOR_PAIR(CP_HEADER));
    wrefresh(w_header);
}

static void draw_mural_panel() {
    draw_border_title(w_mural, " MODULOS ATIVOS ");
    int row = 1;
    
    mural_lock_access();
    
    module_t *cur = mural_peek_list();
    time_t now = time(NULL);
    int maxr = getmaxy(w_mural)-2;
    int idx = 0; // Índice para controle de seleção

    while (cur && row <= maxr) {
        int age = (int)(now - cur->created_at);
        char tbuf[8]; seconds_to_mmss(cur->time_required, tbuf, sizeof(tbuf));
        int remaining_to_timeout = cur->timeout_secs - age;

        // Lógica de Highlight (Seleção)
        int is_selected = (ui_mode == MODE_SEL_MOD && idx == sel_idx);
        
        if (is_selected) {
            wattron(w_mural, A_REVERSE | A_BOLD); // Destaque visual
            mvwprintw(w_mural, row, 1, "->"); // Seta indicativa
        }

        // Cores de status (Timeout)
        if (remaining_to_timeout <= 10) {
            wattron(w_mural, A_BLINK | COLOR_PAIR(CP_ERR));
        } else if (remaining_to_timeout <= 20) {
            wattron(w_mural, COLOR_PAIR(CP_WARN));
        } else {
            wattron(w_mural, COLOR_PAIR(CP_OK));
        }

        int barlen = 12;
        int filled = (cur->time_required > 0) ? (barlen * (cur->time_required - remaining_to_timeout) / (cur->time_required + 1)) : 0;
        if (filled < 0) filled = 0;
        if (filled > barlen) filled = barlen;

        char bar[32]; int p = 0;
        for (int i=0;i<barlen;i++) bar[p++] = (i < filled ? '#' : '.');
        bar[p]=0;

        // Imprime linha do módulo
        mvwprintw(w_mural, row, 4, "ID:%-3d | %-7s | T:%2ds | %s | %2ds",
                  cur->id,
                  (cur->type==MOD_FIOS?"FIOS":(cur->type==MOD_BOTAO?"BOTAO":"SENHAS")),
                  cur->time_required,
                  bar,
                  remaining_to_timeout);

        wattroff(w_mural, A_BLINK | COLOR_PAIR(CP_ERR) | COLOR_PAIR(CP_WARN) | COLOR_PAIR(CP_OK));
        
        if (is_selected) {
            wattroff(w_mural, A_REVERSE | A_BOLD);
        }

        cur = cur->next;
        row++;
        idx++;
    }
    
    mural_unlock_access();
    
    wrefresh(w_mural);
}

static void draw_tedax_panel() {
    draw_border_title(w_tedax, " TEDAX ");
    int row = 1;
    int n = tedax_count();
    
    for (int i=0;i<n;i++) {
        tedax_t *t = tedax_get(i);
        
        // Highlight se estiver selecionando Tedax
        int is_selected = (ui_mode == MODE_SEL_TEDAX && i == sel_idx);
        if (is_selected) wattron(w_tedax, A_REVERSE | A_BOLD);

        pthread_mutex_lock(&t->lock);
        if (t->current) {
            int rem = t->remaining;
            int total = t->current->time_required;
            int barlen = 18;
            int filled = (total > 0) ? (int)((double)(total - rem) / (double)total * barlen + 0.5) : 0;
            if (filled < 0) filled = 0;
            if (filled > barlen) filled = barlen;
            char bar[40]; int p=0;
            for (int k=0;k<barlen;k++) bar[p++] = (k < filled ? '#' : '.');
            bar[p]=0;
            
            if(!is_selected) wattron(w_tedax, COLOR_PAIR(CP_ACCENT));
            mvwprintw(w_tedax, row++, 1, "%sT%d: [O] %s (%2ds) [%s]", 
                      is_selected ? "->" : "  ",
                      t->id,
                      (t->current->type==MOD_FIOS?"FIOS":(t->current->type==MOD_BOTAO?"BOTAO":"SENHAS")),
                      rem, bar);
            if(!is_selected) wattroff(w_tedax, COLOR_PAIR(CP_ACCENT));

        } else {
            if(!is_selected) wattron(w_tedax, COLOR_PAIR(CP_OK));
            mvwprintw(w_tedax, row++, 1, "%sT%d: [ ] Disponivel", is_selected ? "->" : "  ", t->id);
            if(!is_selected) wattroff(w_tedax, COLOR_PAIR(CP_OK));
        }
        pthread_mutex_unlock(&t->lock);

        if (is_selected) wattroff(w_tedax, A_REVERSE | A_BOLD);
    }
    wrefresh(w_tedax);
}

static void draw_bench_panel() {
    draw_border_title(w_bench, " BANCADAS ");
    int occupied_count = 0;
    int n_tedax = tedax_count();
    
    // Calcula ocupação real verificando os Tedax (alternativa seria exportar bench_busy)
    // Para simplificar a UI, vamos listar as bancadas pelo ID
    
    for (int i=0; i<NUM_BENCHES; i++) {
        // Verifica se alguém está usando a bancada i
        int is_busy = 0;
        for(int t=0; t<n_tedax; t++) {
            tedax_t *td = tedax_get(t);
            pthread_mutex_lock(&td->lock);
            if(td->busy && td->bench_id == i) is_busy = 1;
            pthread_mutex_unlock(&td->lock);
        }
        if(is_busy) occupied_count++;

        // Desenha a linha da bancada
        int is_selected = (ui_mode == MODE_SEL_BENCH && i == sel_idx);
        if (is_selected) wattron(w_bench, A_REVERSE | A_BOLD);

        if (is_busy) {
             mvwprintw(w_bench, i+1, 2, "%sBancada %d: [X] OCUPADA", is_selected ? "->" : "  ", i);
        } else {
             mvwprintw(w_bench, i+1, 2, "%sBancada %d: [ ] LIVRE", is_selected ? "->" : "  ", i);
        }

        if (is_selected) wattroff(w_bench, A_REVERSE | A_BOLD);
    }

    // Barra de resumo inferior
    mvwprintw(w_bench, NUM_BENCHES + 2, 2, "Total: %d/%d", occupied_count, NUM_BENCHES);
    wrefresh(w_bench);
}

static void draw_log_panel() {
    draw_border_title(w_log, " LOG ");
    pthread_mutex_lock(&log_lock);
    int maxr = getmaxy(w_log)-2;
    int row = 1;
    int idx = log_pos;
    for (int i=0;i<maxr;i++) {
        if (idx < 0) break;
        char *entry = logbuf[idx];
        if (entry) {
            if (strstr(entry, "x") || strstr(entry, "FALHOU") || strstr(entry, "EXPLODIU"))
                wattron(w_log, COLOR_PAIR(CP_ERR));
            else if (strstr(entry, "ATENCAO") || strstr(entry, "!!") || strstr(entry, "CUIDADO"))
                wattron(w_log, COLOR_PAIR(CP_WARN));
            else
                wattron(w_log, COLOR_PAIR(CP_OK));

            mvwprintw(w_log, row++, 1, "%s", entry);
            wattroff(w_log, COLOR_PAIR(CP_ERR)|COLOR_PAIR(CP_WARN)|COLOR_PAIR(CP_OK));
        }
        idx = (idx - 1 + LOG_LINES) % LOG_LINES;
    }
    pthread_mutex_unlock(&log_lock);
    wrefresh(w_log);
}

static void draw_cmd_panel() {
    draw_border_title(w_cmd, " COMANDOS ");
    
    if (ui_mode == MODE_NORMAL) {
        mvwprintw(w_cmd, 1, 2, "[A] Auto | [D] Selecionar Tarefa (Setas) | [Q] Sair");
    } else if (ui_mode == MODE_SEL_MOD) {
        mvwprintw(w_cmd, 1, 2, "SELECIONE O MODULO (Setas + ENTER)");
    } else if (ui_mode == MODE_SEL_TEDAX) {
        mvwprintw(w_cmd, 1, 2, "SELECIONE O TEDAX (Setas + ENTER)");
    } else if (ui_mode == MODE_SEL_BENCH) {
        mvwprintw(w_cmd, 1, 2, "SELECIONE A BANCADA (Setas + ENTER)");
    }
    
    wrefresh(w_cmd);
}

static void* ui_thread_fn(void *arg) {
    (void)arg;
    initscr();
    start_color();
    use_default_colors();
    
    init_pair(CP_DEFAULT, COLOR_WHITE, -1);
    init_pair(CP_TITLE, COLOR_CYAN, -1);
    init_pair(CP_HEADER, COLOR_YELLOW, -1);
    init_pair(CP_OK, COLOR_GREEN, -1);
    init_pair(CP_WARN, COLOR_YELLOW, -1);
    init_pair(CP_ERR, COLOR_RED, -1);
    init_pair(CP_ACCENT, COLOR_MAGENTA, -1);
    init_pair(CP_SELECT, COLOR_BLACK, COLOR_CYAN);

    noecho();
    cbreak();
    curs_set(0);
    keypad(stdscr, TRUE);
    nodelay(stdscr, TRUE);

    int H = LINES, W = COLS;

    w_header = newwin(1, W, 0, 0);
    w_mural  = newwin(H/2 - 1, W, 1, 0);
    w_tedax  = newwin(H/2 - 3, W/2, H/2, 0);
    w_bench  = newwin(H/2 - 3, W/2, H/2, W/2);
    w_log    = newwin(4, W, H - 7, 0);
    w_cmd    = newwin(3, W, H - 3, 0);

    ui_running = 1;
    while (ui_running) {
        draw_header(W);
        draw_mural_panel();
        draw_tedax_panel();
        draw_bench_panel();
        draw_log_panel();
        draw_cmd_panel();

        int ch = getch();

        // Máquina de Estados da UI
        if (ui_mode == MODE_NORMAL) {
            if (ch == 'q' || ch == 'Q') {
                ui_running = 0;
                break;
            } 
            else if (ch == 'a' || ch == 'A') {
                coord_enqueue_command("A");
                log_event("[UI] Auto-Assign (aleatorio)...");
            } 
            else if (ch == 'd' || ch == 'D') {
                // Inicia modo de seleção visual
                if (mural_count() > 0) {
                    ui_mode = MODE_SEL_MOD;
                    sel_idx = 0;
                } else {
                    log_event("[UI] Mural vazio, nada para selecionar.");
                }
            }
        }
        else if (ui_mode == MODE_SEL_MOD) {
            int count = mural_count();
            if (count == 0) { ui_mode = MODE_NORMAL; continue; }

            if (ch == KEY_UP) sel_idx = (sel_idx - 1 + count) % count;
            else if (ch == KEY_DOWN) sel_idx = (sel_idx + 1) % count;
            else if (ch == 27) ui_mode = MODE_NORMAL; // ESC cancela
            else if (ch == 10) { // ENTER
                // Busca ID do módulo selecionado
                mural_lock_access();
                module_t *m = mural_peek_list();
                for(int k=0; k<sel_idx && m; k++) m = m->next;
                if(m) selected_mod_id = m->id;
                else selected_mod_id = -1;
                mural_unlock_access();

                if(selected_mod_id != -1) {
                    ui_mode = MODE_SEL_TEDAX;
                    sel_idx = 0;
                }
            }
        }
        else if (ui_mode == MODE_SEL_TEDAX) {
            int count = tedax_count();
            if (ch == KEY_UP) sel_idx = (sel_idx - 1 + count) % count;
            else if (ch == KEY_DOWN) sel_idx = (sel_idx + 1) % count;
            else if (ch == 27) ui_mode = MODE_NORMAL;
            else if (ch == 10) {
                selected_tedax_id = sel_idx; // ID é o indice
                ui_mode = MODE_SEL_BENCH;
                sel_idx = 0;
            }
        }
        else if (ui_mode == MODE_SEL_BENCH) {
            int count = NUM_BENCHES;
            if (ch == KEY_UP) sel_idx = (sel_idx - 1 + count) % count;
            else if (ch == KEY_DOWN) sel_idx = (sel_idx + 1) % count;
            else if (ch == 27) ui_mode = MODE_NORMAL;
            else if (ch == 10) {
                selected_bench_id = sel_idx;
                
                // Pede instrução
                werase(w_cmd);
                draw_border_title(w_cmd, " INSTRUCAO ");
                mvwprintw(w_cmd, 1, 2, "M%d -> T%d -> B%d. Digite comando:", selected_mod_id, selected_tedax_id, selected_bench_id);
                wrefresh(w_cmd);
                
                echo(); 
                nodelay(stdscr, FALSE);
                char instr[64];
                wgetnstr(w_cmd, instr, 60);
                noecho();
                nodelay(stdscr, TRUE);
                
                // Envia comando formatado para o coordinator
                char cmd_str[128];
                snprintf(cmd_str, sizeof(cmd_str), "M %d %d %d %s", selected_mod_id, selected_tedax_id, selected_bench_id, instr);
                coord_enqueue_command(cmd_str);
                
                ui_mode = MODE_NORMAL;
            }
        }

        struct timespec ts;
        ts.tv_sec = 0;
        ts.tv_nsec = UI_REFRESH_MS * 1000000;
        nanosleep(&ts, NULL);
    }

    delwin(w_header); delwin(w_mural); delwin(w_tedax);
    delwin(w_bench);  delwin(w_log);   delwin(w_cmd);
    endwin();
    return NULL;
}

void ui_start(void) {
    for (int i=0;i<LOG_LINES;i++) { logbuf[i] = NULL; }
    pthread_create(&ui_thread, NULL, ui_thread_fn, NULL);
    log_event("[SYSTEM] UI iniciado - Use 'D' para selecionar com setas");
}

void ui_stop(void) {
    ui_running = 0;
    pthread_join(ui_thread, NULL);
    pthread_mutex_lock(&log_lock);
    for (int i=0;i<LOG_LINES;i++) {
        if (logbuf[i]) { free(logbuf[i]); logbuf[i] = NULL; }
    }
    pthread_mutex_unlock(&log_lock);
}