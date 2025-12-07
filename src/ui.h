#ifndef UI_H
#define UI_H

void ui_start(void);
void ui_stop(void);

// logging helper accessible by other modules
void log_event(const char *fmt, ...);

#endif // UI_H
