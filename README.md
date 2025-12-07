# Keep Solving and Nobody Explodes — Implementação Concorrente (C / Pthreads / ncurses)
### Trabalho da disciplina de **Programação Concorrente**

---

## 📝 Sobre o Projeto

Este projeto implementa, em linguagem **C**, uma versão concorrente do jogo *Keep Solving and Nobody Explodes*, utilizando múltiplas threads, sincronização via mutexes, semáforos, variáveis de condição e interface baseada em **ncurses**.

O objetivo acadêmico é demonstrar conceitos fundamentais de **programação concorrente**:

- modelo Produtor–Consumidor  
- controle de recursos compartilhados  
- prevenção de race conditions  
- comunicação entre threads  
- execução em tempo real sem travamentos  

---

## 🧵 Arquitetura Concorrente

O sistema utiliza diversas threads em execução simultânea:

| Thread | Responsabilidade |
|--------|------------------|
| **main** | Inicializa estruturas e cria as threads |
| **generator_fn** | Gera módulos dinamicamente (Produtor) |
| **watcher_fn** | Monitora tempo dos módulos e gerencia timeouts |
| **ui_thread_fn** | Interface ncurses + leitura de entrada do jogador |
| **coordinator_fn** | Processa comandos enviados pela UI (Consumidor) |
| **tedax_thread_fn** | Pool de trabalhadores que executam o processamento dos módulos |

Comunicação entre UI → Coordenador segue o padrão **Produtor–Consumidor**, usando mutex + variável de condição.

---

## 🔒 Mecanismos de Sincronização

### ✔ Proteção do Mural (lista de módulos)
```c
pthread_mutex_t mural_lock;
```
A UI utiliza funções seguras:

```c
mural_lock_access();
mural_unlock_access();
```

### ✔ Fila de Comandos
```c
pthread_mutex_t q_mut;
pthread_cond_t  q_cond;
```
Controle de bloqueio com:
```c
pthread_cond_wait(&q_cond, &q_mut);
pthread_cond_signal(&q_cond);
```

### ✔ Bancadas (semáforos)
```c
sem_t benches_sem;
pthread_mutex_t bench_mutex;
```

### ✔ Locks Individuais dos Tedax
```c
pthread_mutex_t lock;
pthread_cond_t cond;
```

---

## 🎮 Como Jogar

Você controla o **Coordenador**.  
Seu objetivo é **manter o sistema estável até o timer principal zerar**.

### Controles:

---

### **A — Auto-Assign**
Atribui automaticamente o módulo mais antigo a um Tedax livre.

```
A
```

---

### **D — Designação Manual**

```
D
<ID do módulo>
[A]ssign ou [S]olve
<comando>
```

---

### Sintaxe dos comandos:

#### FIOS
```
CUT 1
CUT 2
CUT 3
```

#### BOTÃO
```
RED PRESS
BLUE HOLD
GREEN DOUBLE
YELLOW PRESS
```

#### SENHAS
```
WORD FIRE
WORD EARTH
WORD WIND
WORD VOID
```

---

## 🖥 Interface do Jogo

A interface ncurses é dividida em:

- **Mural** → módulos ativos + tempo restante  
- **Tedax** → estado de cada trabalhador  
- **Bancadas** → disponibilidade (.) livre / (#) ocupada  
- **Log** → histórico do sistema  
- **Score** → pontuação total  

Toda a interface roda sem travar nenhuma thread do sistema.

---

## 🧪 Condições do Jogo

### ✔ Vitória
Sobreviver até o cronômetro principal alcançar zero.

### ✔ “Derrota”
Não existe tela de game over.  
Se módulos explodem repetidamente e a fila fica grande demais, o sistema se torna incontrolável.

---

## 📦 Instalação

### Dependências (Ubuntu 24.04)
```bash
sudo apt update
sudo apt install build-essential libncurses5-dev
```

---

## 🔧 Compilação

```bash
make clean
make
```

Gera o executável:

```
./ksne
```

---

## ▶️ Execução

```bash
./ksne
```

Selecione o modo:

- Fácil  
- Médio  
- Difícil  
- Insano  

Cada modo ajusta:

- tempo por módulo  
- taxa de geração  
- número de bancadas  
- número de Tedax  
- duração total da partida  

---

## ⭐ Funcionalidades Implementadas

- Arquitetura concorrente completa  
- Interface ncurses estável e thread-safe  
- Fila sincronizada Produtor–Consumidor  
- Semáforos controlando bancadas  
- Pool de trabalhadores Tedax  
- Sistema de pontuação completo  
- Barras de progresso ASCII  
- Zero warnings de compilação  
- Modo Insano totalmente estável  

---

## 📁 Estrutura do Projeto

```
.
├── src/
│   ├── main.c
│   ├── ui.c
│   ├── coordinator.c
│   ├── generator.c
│   ├── watcher.c
│   ├── tedax.c
│   ├── mural.c
│   └── config.h
├── Makefile
├── README.md
└── docs/
    └── img/   (opcional para screenshots)
```

---

## 👨‍💻 Autor

**Rafael Severo**  
Disciplina de **Programação Concorrente** — 2025  

---

## 📝 Licença

MIT License
