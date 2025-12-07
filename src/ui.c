#define _POSIX_C_SOURCE 200809L
#include "ui.h"
#include "mural.h"
#include "tedax.h"
#include "config.h"

#include <ncurses.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static pthread_t ui_thread;
static volatile int ui_running = 0;

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
enum { CP_DEFAULT=1, CP_TITLE, CP_HEADER, CP_OK, CP_WARN, CP_ERR, CP_ACCENT };

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
    werase(w_header);
    wattron(w_header, A_BOLD | COLOR_PAIR(CP_HEADER));
    mvwprintw(w_header, 0, 2, " LAST SPROUT - BOMB PANEL ");
    wattroff(w_header, A_BOLD | COLOR_PAIR(CP_HEADER));
    // timer center (game clock shown by main thread via log_event)
    wrefresh(w_header);
}

static void draw_mural_panel() {
    draw_border_title(w_mural, " MODULOS ATIVOS ");
    int row = 1;
    module_t *cur = mural_peek_list();
    time_t now = time(NULL);
    int maxr = getmaxy(w_mural)-2;
    while (cur && row <= maxr) {
        int age = (int)(now - cur->created_at);
        char tbuf[8]; seconds_to_mmss(cur->time_required, tbuf, sizeof(tbuf));
        int remaining_to_timeout = cur->timeout_secs - age;

        // choose color/attr for urgent
        if (remaining_to_timeout <= 10) {
            wattron(w_mural, A_BLINK | COLOR_PAIR(CP_ERR));
        } else if (remaining_to_timeout <= 20) {
            wattron(w_mural, COLOR_PAIR(CP_WARN));
        } else {
            wattron(w_mural, COLOR_PAIR(CP_OK));
        }

        // visual bar based on time_required
        int barlen = 12;
        int filled = (cur->time_required > 0) ? (barlen * (cur->time_required - remaining_to_timeout) / (cur->time_required + 1)) : 0;
        if (filled < 0) filled = 0;
        if (filled > barlen) filled = barlen;

        char bar[32]; int p = 0;
        for (int i=0;i<barlen;i++) bar[p++] = (i < filled ? '■' : '·');
        bar[p]=0;

        mvwprintw(w_mural, row++, 1, "ID:%-3d | %-7s | T:%2ds | %s | %2ds",
                  cur->id,
                  (cur->type==MOD_FIOS?"FIOS":(cur->type==MOD_BOTAO?"BOTAO":"SENHAS")),
                  cur->time_required,
                  bar,
                  remaining_to_timeout);

        wattroff(w_mural, A_BLINK | COLOR_PAIR(CP_ERR) | COLOR_PAIR(CP_WARN) | COLOR_PAIR(CP_OK));
        cur = cur->next;
    }
    wrefresh(w_mural);
}

static void draw_tedax_panel() {
    draw_border_title(w_tedax, " TEDAX ");
    int row = 1;
    int n = tedax_count();
    for (int i=0;i<n;i++) {
        tedax_t *t = tedax_get(i);
        pthread_mutex_lock(&t->lock);
        if (t->current) {
            int rem = t->remaining;
            int total = t->current->time_required;
            int barlen = 18;
            int filled = (int)((double)(total - rem) / (double)total * barlen + 0.5);
            if (filled < 0) filled = 0;
            if (filled > barlen) filled = barlen;
            char bar[40]; int p=0;
            for (int k=0;k<barlen;k++) bar[p++] = (k < filled ? '■' : '·');
            bar[p]=0;
            wattron(w_tedax, COLOR_PAIR(CP_ACCENT));
            mvwprintw(w_tedax, row++, 1, "T%d: [●] %s (%2ds left) [%s]", t->id,
                      (t->current->type==MOD_FIOS?"✂ FIOS":(t->current->type==MOD_BOTAO?"⚪ BOTAO":"⌨ SENHAS")),
                      rem, bar);
            wattroff(w_tedax, COLOR_PAIR(CP_ACCENT));
        } else {
            wattron(w_tedax, COLOR_PAIR(CP_OK));
            mvwprintw(w_tedax, row++, 1, "T%d: [○] Disponivel", t->id);
            wattroff(w_tedax, COLOR_PAIR(CP_OK));
        }
        pthread_mutex_unlock(&t->lock);
    }
    wrefresh(w_tedax);
}

static void draw_bench_panel() {
    draw_border_title(w_bench, " BANCADAS ");
    // get number of busy benches by checking tedax occupying benches — we don't have direct benches state here.
    // Show stylized slots: NUM_BENCHES slots, mark as occupied count of active semaphores used.
    // Since sem_t doesn't give value portably, we approximate via tedax busy count.
    int occupied = 0;
    int n = tedax_count();
    for (int i=0;i<n;i++) {
        tedax_t *t = tedax_get(i);
        pthread_mutex_lock(&t->lock);
        if (t->busy && t->current) occupied++;
        pthread_mutex_unlock(&t->lock);
    }
    int freeb = NUM_BENCHES - occupied;
    if (freeb < 0) freeb = 0;
    mvwprintw(w_bench, 1, 2, "BANCADAS OCUPADAS: %d / %d", occupied, NUM_BENCHES);
    // visual bar
    int barlen = 20;
    int p = 0;
    char bar[64];
    for (int i=0;i<NUM_BENCHES && p < barlen; i++) {
        for (int j=0;j<barlen/NUM_BENCHES && p<barlen; j++) {
            bar[p++] = (i < occupied ? '■' : '·');
        }
    }
    bar[p] = 0;
    mvwprintw(w_bench, 3, 2, "[%s]", bar);
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
            // color by symbols in entry
            if (strstr(entry, "✖") || strstr(entry, "FALHOU") || strstr(entry, "ATENCAO"))
                wattron(w_log, COLOR_PAIR(CP_ERR));
            else if (strstr(entry, "ATENÇÃO") || strstr(entry, "!!") || strstr(entry, "CUIDADO"))
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
    mvwprintw(w_cmd, 1, 2, "[A] Auto-assign  |  [D] Designar por ID  |  [Q] Sair");
    mvwprintw(w_cmd, 2, 2, "Modo bomba: vermelho = crítico (timeout <10s)");
    wrefresh(w_cmd);
}

// UI thread main loop
static void* ui_thread_fn(void *arg) {
    (void)arg;
    initscr();
    start_color();
    use_default_colors();
    // color pairs
    init_pair(CP_DEFAULT, COLOR_WHITE, -1);
    init_pair(CP_TITLE, COLOR_CYAN, -1);
    init_pair(CP_HEADER, COLOR_YELLOW, -1);
    init_pair(CP_OK, COLOR_GREEN, -1);
    init_pair(CP_WARN, COLOR_YELLOW, -1);
    init_pair(CP_ERR, COLOR_RED, -1);
    init_pair(CP_ACCENT, COLOR_MAGENTA, -1);

    noecho();
    cbreak();
    curs_set(0);
    keypad(stdscr, TRUE);
    nodelay(stdscr, TRUE);

    int H = LINES, W = COLS;

    // create windows (layout)
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
        if (ch == 'q' || ch == 'Q') {
            ui_running = 0;
            break;
        } else if (ch == 'a' || ch == 'A') {
            // auto assign: pop first module and try assign to first free tedax
            module_t *m = mural_pop_front();
            if (m) {
                int n = tedax_count();
                int assigned = -1;
                for (int i=0;i<n;i++) {
                    if (tedax_assign_module(i, m) == 0) { assigned = i; break; }
                }
                if (assigned >= 0) log_event("[UI] Auto-assign M%d → T%d", m->id, assigned);
                else {
                    mural_requeue(m);
                    log_event("[UI] Auto-assign falhou: nenhum Tédax livre");
                }
            } else {
                log_event("[UI] Auto-assign: mural vazio");
            }
        } else if (ch == 'd' || ch == 'D') {
    // prompt blocking for ID
    echo();
    nodelay(stdscr, FALSE);
    char s[64];
    werase(w_cmd);
    draw_border_title(w_cmd, " COMANDOS ");
    mvwprintw(w_cmd, 1, 2, "Digite ID do modulo e ENTER: ");
    wrefresh(w_cmd);
    wgetnstr(w_cmd, s, 15);
    int id = atoi(s);

    // pop the module by id (non-blocking)
    module_t *m = mural_pop_by_id(id);
    if (!m) {
        log_event("[UI] M%d nao encontrado no mural", id);
        noecho();
        nodelay(stdscr, TRUE);
    } else {
        // ask assign or solve
        werase(w_cmd);
        draw_border_title(w_cmd, " MODULO ");
        mvwprintw(w_cmd, 1, 2, "M%d selecionado. [A]ssign p/ Tedax  [S]olve agora", m->id);
        wrefresh(w_cmd);
        int choice = 0;
        while (1) {
            int c = wgetch(w_cmd);
            if (c == 'a' || c == 'A') { choice = 'A'; break; }
            if (c == 's' || c == 'S') { choice = 'S'; break; }
            // small sleep to avoid busy loop
            struct timespec t; t.tv_sec=0; t.tv_nsec=100*1000000; nanosleep(&t,NULL);
        }

        if (choice == 'A') {
            // Assign to Tedax: ask instruction to pass
            werase(w_cmd);
            draw_border_title(w_cmd, " DESIGNAR ");
            mvwprintw(w_cmd, 1, 2, "Digite instrução para o Tedax (ex: CUT 2 | BLUE HOLD | WORD CAT): ");
            wrefresh(w_cmd);
            char instr[64];
            wgetnstr(w_cmd, instr, 60);
            // store instruction in module
            snprintf(m->instruction, sizeof(m->instruction), "%s", instr);

            // assign to first free tedax
            int n = tedax_count();
            int assigned = -1;
            for (int i=0;i<n;i++) {
                if (tedax_assign_module(i, m) == 0) { assigned = i; break; }
            }
            if (assigned >= 0) {
                log_event("[UI] M%d → T%d (instr: %s)", m->id, assigned, m->instruction);
            } else {
                mural_requeue(m);
                log_event("[UI] nenhum TÉDAX livre — re-enfileirado M%d", m->id);
            }
            noecho();
            nodelay(stdscr, TRUE);
        } else {
            // Solve now (player solves interactively)
            werase(w_cmd);
            draw_border_title(w_cmd, " RESOLVER AGORA ");
            // present minigame according to type
            if (m->type == MOD_FIOS) {
                mvwprintw(w_cmd,1,2,"M%d - FIOS: existem N fios. Instrução esperada: CUT <n>", m->id);
                mvwprintw(w_cmd,2,2,"Digite instrução (ex: CUT 2): ");
                wrefresh(w_cmd);
                char ans[64];
                wgetnstr(w_cmd, ans, 60);
                // compare answer with solution (case-insensitive)
                char a_low[128], s_low[128];
                snprintf(a_low,sizeof(a_low),"%s",ans);
                snprintf(s_low,sizeof(s_low),"%s",m->solution);
                for (char *p=a_low; *p; ++p) if (*p>='A' && *p<='Z') *p = *p - 'A' + 'a';
                for (char *p=s_low; *p; ++p) if (*p>='A' && *p<='Z') *p = *p - 'A' + 'a';
                if (strcmp(a_low, s_low) == 0) {
                    log_event("[PLAYER] M%d resolvido com sucesso (YOU)", m->id);
                    free(m);
                } else {
                    log_event("[PLAYER] M%d resolucao INCORRETA — requeue", m->id);
                    mural_requeue(m);
                }
            } else if (m->type == MOD_BOTAO) {
                mvwprintw(w_cmd,1,2,"M%d - BOTAO: instrução esperada (ex: BLUE HOLD or RED PRESS)", m->id);
                mvwprintw(w_cmd,2,2,"Digite instrução: ");
                wrefresh(w_cmd);
                char ans[64];
                wgetnstr(w_cmd, ans, 60);
                char a_low[128], s_low[128];
                snprintf(a_low,sizeof(a_low),"%s",ans);
                snprintf(s_low,sizeof(s_low),"%s",m->solution);
                for (char *p=a_low; *p; ++p) if (*p>='A' && *p<='Z') *p = *p - 'A' + 'a';
                for (char *p=s_low; *p; ++p) if (*p>='A' && *p<='Z') *p = *p - 'A' + 'a';
                if (strcmp(a_low, s_low) == 0) {
                    log_event("[PLAYER] M%d resolvido com sucesso (YOU)", m->id);
                    free(m);
                } else {
                    log_event("[PLAYER] M%d resolucao INCORRETA — requeue", m->id);
                    mural_requeue(m);
                }
            } else { // SENHAS
                mvwprintw(w_cmd,1,2,"M%d - SENHAS: digite a palavra correta (ex: WORD CAT -> responda WORD CAT)", m->id);
                mvwprintw(w_cmd,2,2,"Digite instrução: ");
                wrefresh(w_cmd);
                char ans[64];
                wgetnstr(w_cmd, ans, 60);
                char a_low[128], s_low[128];
                snprintf(a_low,sizeof(a_low),"%s",ans);
                snprintf(s_low,sizeof(s_low),"%s",m->solution);
                for (char *p=a_low; *p; ++p) if (*p>='A' && *p<='Z') *p = *p - 'A' + 'a';
                for (char *p=s_low; *p; ++p) if (*p>='A' && *p<='Z') *p = *p - 'A' + 'a';
                if (strcmp(a_low, s_low) == 0) {
                    log_event("[PLAYER] M%d resolvido com sucesso (YOU)", m->id);
                    free(m);
                } else {
                    log_event("[PLAYER] M%d resolucao INCORRETA — requeue", m->id);
                    mural_requeue(m);
                }
            } // end type cases
            noecho();
            nodelay(stdscr, TRUE);
        } // end if assign/solve
    } // end if m exists
} // end case D

        struct timespec ts;
        ts.tv_sec = 0;
        ts.tv_nsec = UI_REFRESH_MS * 1000000;
        nanosleep(&ts, NULL);
    }

    // cleanup windows
    delwin(w_header);
    delwin(w_mural);
    delwin(w_tedax);
    delwin(w_bench);
    delwin(w_log);
    delwin(w_cmd);
    endwin();
    return NULL;
}

void ui_start(void) {
    // init log buffer
    for (int i=0;i<LOG_LINES;i++) { logbuf[i] = NULL; }
    pthread_create(&ui_thread, NULL, ui_thread_fn, NULL);
    log_event("[SYSTEM] UI iniciado");
}

void ui_stop(void) {
    ui_running = 0;
    pthread_join(ui_thread, NULL);

    // free log entries
    pthread_mutex_lock(&log_lock);
    for (int i=0;i<LOG_LINES;i++) {
        if (logbuf[i]) { free(logbuf[i]); logbuf[i] = NULL; }
    }
    pthread_mutex_unlock(&log_lock);
}
