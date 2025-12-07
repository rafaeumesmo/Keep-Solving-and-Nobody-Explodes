#ifndef MURAL_H
#define MURAL_H

#include <time.h>

typedef enum { MOD_FIOS=0, MOD_BOTAO=1, MOD_SENHAS=2 } module_type_t;

typedef struct module {
    int id;
    module_type_t type;
    int time_required;
    time_t created_at;
    int timeout_secs;

    // NOVOS CAMPOS:
    char solution[64];     // string que representa a solução correta do módulo
    char instruction[64];  // string com a instrução dada pelo coordenador (p/ Tedax)

    struct module *next;
} module_t;


void mural_init(void);
void mural_destroy(void);

module_t* create_module(int id);
void mural_push(module_t *m);
module_t* mural_pop_front(void);
module_t* mural_pop_by_id(int id);
void mural_requeue(module_t *m);
module_t* mural_peek_list(void);
int mural_count(void); 
module_t* mural_pop(void);
module_t* mural_find_by_tedax_type(int tedax_id, char type);

#endif // MURAL_H
