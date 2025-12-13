# Keep Solving and Nobody Explodes — Implementação Concorrente

### Trabalho da disciplina de **Programação Concorrente** — IDP 2025

---

## 📝 Sobre o Projeto

Este projeto consiste numa simulação concorrente do jogo *Keep Solving and Nobody Explodes*, desenvolvida em **Linguagem C** para ambiente Linux. O sistema utiliza a biblioteca **POSIX Threads (pthreads)** para gerir múltiplos agentes independentes e uma interface gráfica interativa baseada em **ncurses**.

O objetivo acadêmico é demonstrar a aplicação prática de:
- **Modelo Produtor–Consumidor**
- **Sincronização de Threads** (Mutexes, Variáveis de Condição)
- **Gestão de Recursos Limitados** (Semáforos)
- **Prevenção de Condições de Corrida** (*Race Conditions*)

---

## 🚀 Novidades da Versão 2.0

- **Sistema de Menus:** Navegação interativa (Menu Principal, Seleção de Dificuldade) antes do início da partida.
- **Painel de Resolvidos:** Histórico visual em tempo real dos módulos desarmados com sucesso.
- **Input Bufferizado:** Digitação de comandos complexos sem bloquear a renderização da interface.
- **Condição de Vitória:** O jogo encerra com sucesso ao atingir a meta de pontuação definida.

---

## 🧵 Arquitetura do Sistema

O software opera com múltiplas threads em paralelo, divididas por responsabilidades:

| Thread | Função |
| :--- | :--- |
| **Main Thread** | Gerencia o ciclo de vida (menus), o *timer* global e verifica a condição de vitória/derrota. |
| **Generator** | Thread *Produtora*. Cria periodicamente novos módulos explosivos e insere-os no Mural. |
| **Watcher** | Thread *Monitora*. Verifica o tempo de vida dos módulos ativos e aplica penalidades em caso de explosão (*timeout*). |
| **UI Thread** | Thread de *Interface*. Renderiza os painéis (ncurses) e captura o input do utilizador num *buffer* local. |
| **Coordinator** | Thread *Consumidora*. Processa a fila de comandos enviada pela UI e delega tarefas aos técnicos. |
| **Tedax Pool** | Conjunto de threads *Trabalhadoras*. Simulam os técnicos que competem pelo acesso às bancadas físicas. |

---

## 🔒 Mecanismos de Sincronização

A integridade do sistema é garantida por primitivas de sincronização POSIX:

### 1. Proteção de Dados (Mutex)
- **`mural_lock`:** Protege o acesso às listas encadeadas de módulos (Ativos e Resolvidos). Impede que a UI leia a lista enquanto o Gerador ou um Tedax a modifica.
- **`q_mut`:** Protege a fila de comandos entre a UI e o Coordenador.

### 2. Gestão de Recursos (Semáforos)
- **`benches_sem`:** Controla o acesso às **Bancadas** (recursos físicos limitados).
- **Lógica de Assimetria:** Se houver mais Técnicos (Tedax) do que Bancadas, os técnicos excedentes bloqueiam no semáforo até que uma bancada seja libertada.

### 3. Comunicação (Variáveis de Condição)
- **`q_cond`:** Permite que o Coordenador "durma" enquanto a fila de comandos estiver vazia, acordando apenas quando a UI sinalizar um novo comando.
- **Tedax Cond:** Cada técnico tem a sua própria variável de condição para aguardar a atribuição de tarefas.

---

## 🎮 Manual do Jogador

Você atua como **Coordenador**. O seu objetivo é gerir a equipa para desarmar módulos suficientes antes que o tempo acabe.

### Navegação nos Menus
Ao iniciar o jogo (`./ksne`):
1. Use as **SETAS** (`↑` `↓`) para selecionar **"Modo Clássico"** e tecle **ENTER**.
2. Selecione a **Dificuldade** (Fácil, Médio, Difícil, Insano) e tecle **ENTER**.

### Interface Principal
A tela de jogo é dividida em:
- **Esquerda (Mural):** Módulos pendentes (Vermelho = Crítico).
- **Direita (Resolvidos):** Histórico de módulos desarmados.
- **Centro:** Status dos Tedax e Bancadas.
- **Rodapé:** Log de eventos e campo de Input.

### Comandos de Jogo

#### **A — Auto-Assign**
O sistema tenta atribuir automaticamente o módulo mais antigo a qualquer técnico livre.
* **Tecla:** `A`

#### **D — Designação Manual (Fluxo Interativo)**
Permite o controlo preciso da operação.
1.  Pressione `D`.
2.  Use as **Setas** para escolher o **Módulo** (no Mural) $\to$ `ENTER`.
3.  Escolha o **Técnico** (Tedax) $\to$ `ENTER`.
4.  Escolha a **Bancada** $\to$ `ENTER`.
5.  O cursor irá para o campo `Instrucao:`. Digite a senha de desarmamento e tecle `ENTER`.

---

### ⌨️ Lista de Senhas de Desarmamento

No modo manual, é necessário digitar o comando correto conforme o tipo de módulo:

| Tipo do Módulo | Sintaxe | Exemplos Válidos |
| :--- | :--- | :--- |
| **🔌 FIOS** | `CUT <número>` | `CUT 1` <br> `CUT 2` <br> `CUT 3` |
| **🔴 BOTÃO** | `<COR> <AÇÃO>` | `RED PRESS` <br> `BLUE HOLD` <br> `GREEN DOUBLE` |
| **🔡 SENHAS** | `WORD <PALAVRA>` | `WORD FIRE` <br> `WORD WATER` <br> `WORD VOID` |

> **Nota:** O sistema ignora maiúsculas e minúsculas (ex: `cut 1` funciona).

---

## 📦 Instalação e Execução

### Requisitos (Linux / Ubuntu 24.04)
É necessário ter o compilador GCC e as bibliotecas de desenvolvimento do ncurses.

```bash
sudo apt update
sudo apt install build-essential libncurses5-dev
