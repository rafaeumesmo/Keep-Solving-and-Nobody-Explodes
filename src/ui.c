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
#include <ctype.h>

// Cores e Pares
enum { CP_DEFAULT=1, CP_TITLE, CP_HEADER, CP_OK, CP_WARN, CP_ERR, CP_ACCENT, CP_SELECT };

static pthread_t ui_thread;
static volatile int ui_running = 0;

// Estados da Interface In-Game
enum { 
    MODE_NORMAL=0, 
    MODE_SEL_MOD, 
    MODE_SEL_TEDAX, 
    MODE_SEL_BENCH,
    MODE_INPUT_CMD 
};

static int ui_mode = MODE_NORMAL;
static int sel_idx = 0;
static int selected_mod_id = -1;
static int selected_tedax_id = -1;
static int selected_bench_id = -1;

static char input_buf[64];
static int input_pos = 0;

// Windows (Adicionada w_completed)
static WINDOW *w_header, *w_mural, *w_completed, *w_tedax, *w_bench, *w_log, *w_cmd;

// Log buffer
static char *logbuf[LOG_LINES];
static int log_pos = -1;
static pthread_mutex_t log_lock = PTHREAD_MUTEX_INITIALIZER;

// --- FUNÇÕES DE ESTADO ---
int is_ui_active(void) { return ui_running; }

// --- MENUS ---
void show_start_screen(void) {
    initscr(); start_color(); noecho(); curs_set(0); keypad(stdscr, TRUE);
    init_pair(1, COLOR_WHITE, COLOR_BLACK);
    init_pair(2, COLOR_CYAN, COLOR_BLACK);
    const char *title = "KEEP SOLVING!!";
    const char *press = "Pressione qualquer tecla";
    const char *footer = "© 2025 IDP - Keep Solving and Nobody Explodes";
    int blink = 0; nodelay(stdscr, TRUE);
    while (1) {
        clear(); int h, w; getmaxyx(stdscr, h, w);
        attron(A_BOLD | COLOR_PAIR(2));
        mvprintw(h/2-2, (w-(int)strlen(title))/2, "%s", title);
        attroff(A_BOLD | COLOR_PAIR(2));
        if (blink < 10) mvprintw(h/2, (w-(int)strlen(press))/2, "%s", press);
        mvprintw(h-2, (w-(int)strlen(footer))/2, "%s", footer);
        refresh();
        if (getch() != ERR) break;
        struct timespec ts = {0, 50000000}; nanosleep(&ts, NULL);
        blink = (blink + 1) % 20;
    }
    nodelay(stdscr, FALSE); endwin();
}

int show_main_menu_ncurses(void) {
    initscr(); start_color(); noecho(); curs_set(0); keypad(stdscr, TRUE);
    const char *options[] = {"Modo Classico", "Opcoes (N/A)", "Sair"};
    int n=3, sel=0;
    while(1) {
        erase(); int h, w; getmaxyx(stdscr,h,w);
        attron(A_BOLD); mvprintw(3, (w-13)/2, "KEEP SOLVING!"); attroff(A_BOLD);
        for(int i=0;i<n;i++) {
            if(i==sel) attron(A_REVERSE|A_BOLD);
            mvprintw(h/2-2 + i*2, w/2-10, " %s ", options[i]);
            if(i==sel) attroff(A_REVERSE|A_BOLD);
        }
        mvprintw(h-2, 2, "[SETAS] Navegar  [ENTER] Selecionar");
        refresh();
        int ch = getch();
        if(ch==KEY_UP) { sel--; if(sel<0) sel=n-1; }
        else if(ch==KEY_DOWN) { sel++; if(sel>=n) sel=0; }
        else if(ch==10) { endwin(); return sel; }
    }
}

int show_difficulty_menu_ncurses(void) {
    initscr(); start_color(); noecho(); curs_set(0); keypad(stdscr, TRUE);
    const char *options[] = {"[1] Facil", "[2] Medio", "[3] Dificil", "[4] Insano"};
    int n=4, sel=1;
    while(1) {
        erase(); int h, w; getmaxyx(stdscr,h,w);
        attron(A_BOLD); mvprintw(3, (w-23)/2, "SELECIONE A DIFICULDADE"); attroff(A_BOLD);
        for(int i=0;i<n;i++) {
            if(i==sel) attron(A_REVERSE|A_BOLD);
            mvprintw(h/2-2 + i*2, w/2-10, " %s ", options[i]);
            if(i==sel) attroff(A_REVERSE|A_BOLD);
        }
        refresh();
        int ch = getch();
        if(ch==KEY_UP) { sel--; if(sel<0) sel=n-1; }
        else if(ch==KEY_DOWN) { sel++; if(sel>=n) sel=0; }
        else if(ch==10) { endwin(); return sel+1; }
    }
}

// --- LOGGING ---
void log_event(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    char tmp[256]; vsnprintf(tmp, sizeof(tmp), fmt, ap); va_end(ap);
    pthread_mutex_lock(&log_lock);
    log_pos = (log_pos + 1) % LOG_LINES;
    if (logbuf[log_pos]) free(logbuf[log_pos]);
    char *entry = malloc(320);
    time_t t = time(NULL);
    struct tm tm = *localtime(&t);
    snprintf(entry, 320, "[%02d:%02d:%02d] %s", tm.tm_hour, tm.tm_min, tm.tm_sec, tmp);
    logbuf[log_pos] = entry;
    pthread_mutex_unlock(&log_lock);
}

// --- UI DRAWING ---
static void draw_border_title(WINDOW *w, const char *title) {
    werase(w); box(w, 0, 0);
    wattron(w, A_BOLD | COLOR_PAIR(CP_TITLE));
    mvwprintw(w, 0, 2, " %s ", title);
    wattroff(w, A_BOLD | COLOR_PAIR(CP_TITLE));
}

static void seconds_to_mmss(int s, char *buf, int len) {
    int m=s/60; int sec=s%60; snprintf(buf, len, "%02d:%02d", m, sec);
}

static void draw_header(int cols) {
    (void)cols; werase(w_header);
    wattron(w_header, A_BOLD | COLOR_PAIR(CP_HEADER));
    int rem = mural_get_remaining_seconds();
    char tbuf[16]; seconds_to_mmss(rem, tbuf, sizeof(tbuf));
    mvwprintw(w_header, 0, 1, " Keep Solving - BOMB PANEL | SCORE: %d | GOLD: %d | TIME: %s ", 
              mural_get_score(), mural_get_money(), tbuf);
    wattroff(w_header, A_BOLD | COLOR_PAIR(CP_HEADER)); wrefresh(w_header);
}

static void draw_mural_panel() {
    draw_border_title(w_mural, " ATIVOS ");
    int row = 1;
    mural_lock_access();
    module_t *cur = mural_peek_list();
    time_t now = time(NULL);
    int maxr = getmaxy(w_mural)-2;
    int idx = 0;
    while (cur && row <= maxr) {
        int age = (int)(now - cur->created_at);
        int rem = cur->timeout_secs - age;
        if (ui_mode == MODE_SEL_MOD && idx == sel_idx) {
            wattron(w_mural, A_REVERSE | A_BOLD); mvwprintw(w_mural, row, 1, "->");
        } else mvwprintw(w_mural, row, 1, "  ");

        if (rem<=10) wattron(w_mural, A_BLINK|COLOR_PAIR(CP_ERR));
        else if (rem<=20) wattron(w_mural, COLOR_PAIR(CP_WARN));
        else wattron(w_mural, COLOR_PAIR(CP_OK));

        int barlen=8; // Menor barra pois a tela é menor
        int filled = (cur->time_required>0)?(barlen*(cur->time_required-rem)/cur->time_required):0;
        if(filled < 0) { filled = 0; } 
        if(filled > barlen) { filled = barlen; }
        char bar[32]; int p=0; for(int k=0;k<barlen;k++) bar[p++]=(k<filled?'#':'.'); bar[p]=0;
        
        mvwprintw(w_mural, row, 4, "M%-2d|%-6s|%s|%2ds",
                  cur->id, (cur->type==MOD_FIOS?"FIOS":(cur->type==MOD_BOTAO?"BOTAO":"SENHA")), bar, rem);
        wattroff(w_mural, A_BLINK|COLOR_PAIR(CP_ERR)|COLOR_PAIR(CP_WARN)|COLOR_PAIR(CP_OK));
        if (ui_mode == MODE_SEL_MOD && idx == sel_idx) wattroff(w_mural, A_REVERSE | A_BOLD);
        cur=cur->next; row++; idx++;
    }
    mural_unlock_access(); wrefresh(w_mural);
}

// --- NOVO: PAINEL DE RESOLVIDOS ---
static void draw_completed_panel() {
    draw_border_title(w_completed, " RESOLVIDOS ");
    int row = 1;
    mural_lock_access();
    module_t *cur = mural_peek_resolved();
    int maxr = getmaxy(w_completed)-2;
    
    while (cur && row <= maxr) {
        wattron(w_completed, COLOR_PAIR(CP_OK));
        mvwprintw(w_completed, row, 2, "M%-2d [OK] %s", 
                  cur->id, 
                  (cur->type==MOD_FIOS?"FIOS":(cur->type==MOD_BOTAO?"BOTAO":"SENHA")));
        wattroff(w_completed, COLOR_PAIR(CP_OK));
        cur = cur->next;
        row++;
    }
    mural_unlock_access(); 
    wrefresh(w_completed);
}

static void draw_tedax_panel() {
    draw_border_title(w_tedax, " TEDAX ");
    int row = 1; int n = tedax_count();
    for (int i=0;i<n;i++) {
        tedax_t *t = tedax_get(i);
        int is_sel = (ui_mode == MODE_SEL_TEDAX && i == sel_idx);
        if (is_sel) wattron(w_tedax, A_REVERSE | A_BOLD);
        pthread_mutex_lock(&t->lock);
        if (t->current) {
            wattron(w_tedax, COLOR_PAIR(CP_ACCENT));
            mvwprintw(w_tedax, row++, 1, "%sT%d: [O] %s (%2ds)", is_sel?"->":"  ", t->id, 
                      (t->current->type==MOD_FIOS?"FIOS":(t->current->type==MOD_BOTAO?"BOTAO":"SENHA")), t->remaining);
            wattroff(w_tedax, COLOR_PAIR(CP_ACCENT));
        } else {
            wattron(w_tedax, COLOR_PAIR(CP_OK));
            mvwprintw(w_tedax, row++, 1, "%sT%d: [ ] LIVRE", is_sel?"->":"  ", t->id);
            wattroff(w_tedax, COLOR_PAIR(CP_OK));
        }
        pthread_mutex_unlock(&t->lock);
        if (is_sel) wattroff(w_tedax, A_REVERSE | A_BOLD);
    }
    wrefresh(w_tedax);
}

static void draw_bench_panel() {
    draw_border_title(w_bench, " BANCADAS ");
    int n_tedax = tedax_count();
    for (int i=0; i<NUM_BENCHES; i++) {
        int is_busy = 0;
        for(int t=0; t<n_tedax; t++) {
            tedax_t *td = tedax_get(t);
            pthread_mutex_lock(&td->lock);
            if(td->busy && td->bench_id == i) is_busy = 1;
            pthread_mutex_unlock(&td->lock);
        }
        int is_sel = (ui_mode == MODE_SEL_BENCH && i == sel_idx);
        if (is_sel) wattron(w_bench, A_REVERSE);
        if (is_busy) {
             wattron(w_bench, COLOR_PAIR(CP_ERR));
             mvwprintw(w_bench, i+1, 2, "%sBancada %d: [X] OCUPADA", is_sel?"->":"  ", i);
             wattroff(w_bench, COLOR_PAIR(CP_ERR));
        } else {
             wattron(w_bench, COLOR_PAIR(CP_OK));
             mvwprintw(w_bench, i+1, 2, "%sBancada %d: [ ] LIVRE", is_sel?"->":"  ", i);
             wattroff(w_bench, COLOR_PAIR(CP_OK));
        }
        if (is_sel) wattroff(w_bench, A_REVERSE);
    }
    wrefresh(w_bench);
}

static void draw_log_panel() {
    draw_border_title(w_log, " LOG ");
    pthread_mutex_lock(&log_lock);
    int maxr = getmaxy(w_log)-2; int row = 1; int idx = log_pos;
    for (int i=0;i<maxr;i++) {
        if (idx < 0) break;
        char *e = logbuf[idx];
        if (e) {
            if (strstr(e,"EXPLODIU")||strstr(e,"FALHOU")||strstr(e,"timeout")) wattron(w_log, COLOR_PAIR(CP_ERR));
            else if (strstr(e,"DESARMADO")||strstr(e,"sucesso")) wattron(w_log, COLOR_PAIR(CP_OK));
            else if (strstr(e,"Auto")||strstr(e,"ASSIGN")) wattron(w_log, COLOR_PAIR(CP_ACCENT));
            mvwprintw(w_log, row++, 1, "%s", e);
            wattroff(w_log, COLOR_PAIR(CP_ERR)|COLOR_PAIR(CP_OK)|COLOR_PAIR(CP_ACCENT));
        }
        idx = (idx-1+LOG_LINES)%LOG_LINES;
    }
    pthread_mutex_unlock(&log_lock); wrefresh(w_log);
}

static void draw_cmd_panel() {
    draw_border_title(w_cmd, " COMANDOS ");
    if (ui_mode==MODE_NORMAL) {
        mvwprintw(w_cmd, 1, 2, "[A] Auto | [D] Selecionar | [Q] Menu Principal");
    } else if (ui_mode==MODE_SEL_MOD) {
        mvwprintw(w_cmd, 1, 2, "SELECIONE MODULO | [Q] Cancelar");
    } else if (ui_mode==MODE_SEL_TEDAX) {
        mvwprintw(w_cmd, 1, 2, "SELECIONE TEDAX");
    } else if (ui_mode==MODE_SEL_BENCH) {
        mvwprintw(w_cmd, 1, 2, "SELECIONE BANCADA");
    } else if (ui_mode==MODE_INPUT_CMD) {
        mvwprintw(w_cmd, 1, 2, "Instrucao: %s_", input_buf);
    }
    wrefresh(w_cmd);
}

static void* ui_thread_fn(void *arg) {
    (void)arg;
    initscr(); start_color(); use_default_colors();
    init_pair(CP_DEFAULT, COLOR_WHITE, -1);
    init_pair(CP_TITLE, COLOR_CYAN, -1);
    init_pair(CP_HEADER, COLOR_YELLOW, -1);
    init_pair(CP_OK, COLOR_GREEN, -1);
    init_pair(CP_WARN, COLOR_YELLOW, -1);
    init_pair(CP_ERR, COLOR_RED, -1);
    init_pair(CP_ACCENT, COLOR_MAGENTA, -1);
    init_pair(CP_SELECT, COLOR_BLACK, COLOR_CYAN);
    noecho(); cbreak(); curs_set(0); keypad(stdscr, TRUE); nodelay(stdscr, TRUE);

    int H = LINES, W = COLS;
    // Layout novo: Metade esquerda p/ mural, metade direita p/ resolvidos
    w_header = newwin(1, W, 0, 0);
    w_mural  = newwin(H/2 - 1, W/2, 1, 0);
    w_completed = newwin(H/2 - 1, W - W/2, 1, W/2); // Nova janela
    w_tedax  = newwin(H/2 - 3, W/2, H/2, 0);
    w_bench  = newwin(H/2 - 3, W/2, H/2, W/2);
    w_log    = newwin(4, W, H - 7, 0);
    w_cmd    = newwin(3, W, H - 3, 0);
    
    keypad(w_cmd, TRUE);

    ui_running = 1;
    while (ui_running) {
        // Agora desenhamos também o painel de resolvidos
        draw_header(W); draw_mural_panel(); draw_completed_panel();
        draw_tedax_panel(); draw_bench_panel(); draw_log_panel(); draw_cmd_panel();
        
        int ch = getch();
        
        if (ui_mode == MODE_INPUT_CMD) {
            if (ch != ERR) {
                if (ch == '\n' || ch == KEY_ENTER || ch == 10 || ch == 13) {
                    char cmd[128]; 
                    snprintf(cmd, 128, "M %d %d %d %s", selected_mod_id, selected_tedax_id, selected_bench_id, input_buf);
                    coord_enqueue_command(cmd); 
                    ui_mode = MODE_NORMAL;
                    input_pos = 0; input_buf[0] = '\0';
                }
                else if (ch == KEY_BACKSPACE || ch == 127 || ch == '\b') {
                    if (input_pos > 0) {
                        input_buf[--input_pos] = '\0';
                    }
                }
                else if (ch == 27) { 
                    ui_mode = MODE_NORMAL;
                    input_pos = 0; input_buf[0] = '\0';
                }
                else if (isprint(ch) && input_pos < 60) {
                    input_buf[input_pos++] = (char)ch;
                    input_buf[input_pos] = '\0';
                }
            }
        }
        else if (ui_mode == MODE_NORMAL) {
            if (ch == 'q' || ch == 'Q') { ui_running = 0; break; }
            else if (ch == 'a' || ch == 'A') { coord_enqueue_command("A"); log_event("[UI] Auto-assign"); }
            else if (ch == 'd' || ch == 'D') { 
                if (mural_count()>0) { ui_mode=MODE_SEL_MOD; sel_idx=0; } else log_event("[UI] Mural vazio!");
            }
        }
        else if (ui_mode == MODE_SEL_MOD) {
            int cnt = mural_count(); if(cnt==0) { ui_mode=MODE_NORMAL; continue; }
            if (ch==KEY_UP) sel_idx=(sel_idx-1+cnt)%cnt;
            else if (ch==KEY_DOWN) sel_idx=(sel_idx+1)%cnt;
            else if (ch=='q'||ch=='Q'||ch==27) ui_mode=MODE_NORMAL;
            else if (ch==10 || ch==KEY_ENTER || ch==13) {
                 module_t *m = mural_get_by_index(sel_idx); 
                 if(m) selected_mod_id = m->id;
                 ui_mode=MODE_SEL_TEDAX; sel_idx=0;
            }
        }
        else if (ui_mode == MODE_SEL_TEDAX) {
             int cnt=tedax_count();
             if (ch==KEY_UP) sel_idx=(sel_idx-1+cnt)%cnt;
             else if (ch==KEY_DOWN) sel_idx=(sel_idx+1)%cnt;
             else if (ch==10 || ch==KEY_ENTER || ch==13) { selected_tedax_id=sel_idx; ui_mode=MODE_SEL_BENCH; sel_idx=0; }
        }
        else if (ui_mode == MODE_SEL_BENCH) {
             int cnt=NUM_BENCHES;
             if (ch==KEY_UP) sel_idx=(sel_idx-1+cnt)%cnt;
             else if (ch==KEY_DOWN) sel_idx=(sel_idx+1)%cnt;
             else if (ch==10 || ch==KEY_ENTER || ch==13) {
                 selected_bench_id=sel_idx;
                 ui_mode = MODE_INPUT_CMD;
                 input_pos = 0; input_buf[0] = '\0';
             }
        }
        
        struct timespec ts = {0, 100000000}; 
        nanosleep(&ts, NULL);
    }
    
    delwin(w_header); delwin(w_mural); delwin(w_completed); 
    delwin(w_tedax); delwin(w_bench); delwin(w_log); delwin(w_cmd);
    endwin(); return NULL;
}

void ui_start(void) {
    pthread_mutex_lock(&log_lock);
    for(int i=0; i<LOG_LINES; i++) { 
        if(logbuf[i]) { free(logbuf[i]); logbuf[i]=NULL; } 
    }
    log_pos = -1;
    pthread_mutex_unlock(&log_lock);

    pthread_create(&ui_thread, NULL, ui_thread_fn, NULL);
    log_event("[SYSTEM] UI Iniciada (Keep Solving and Nobody Explodes)");
}

void ui_stop(void) {
    ui_running = 0;
    pthread_join(ui_thread, NULL);
    pthread_mutex_lock(&log_lock);
    for(int i=0; i<LOG_LINES; i++) { 
        if(logbuf[i]) { free(logbuf[i]); logbuf[i]=NULL; } 
    }
    pthread_mutex_unlock(&log_lock);
}