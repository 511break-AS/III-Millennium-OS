// Ante-Millennium Operating System - antem_stdio
// Copyright (C) 2026  Alberto Sanfelice

// This program is free software; you can redistribute it and/or
// modify it under the terms of the GNU General Public License
// as published by the Free Software Foundation; version 2
// of the License.

// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.

// You should have received a copy of the GNU General Public License
// along with this program; if not, see
// <https://www.gnu.org/licenses/>.

// antem_libc/stdio.h
// La Libreria Standard I/O per Ante-Millennium OS

#ifndef _ANTEM_STDIO_H
#define _ANTEM_STDIO_H
#define BUTTON_DEFAULT 0
#include "stddef.h" 

// --- SISTEMA DI ANCORAGGIO GUI (Griglia a 9 punti) ---
#define ANCHOR_TL 0 // In alto a Sinistra (Top-Left)
#define ANCHOR_TR 1 // In alto a Destra (Top-Right)
#define ANCHOR_BL 2 // In basso a Sinistra (Bottom-Left)
#define ANCHOR_BR 3 // In basso a Destra (Bottom-Right)
#define ANCHOR_TC 4 // In alto al Centro (Top-Center)
#define ANCHOR_BC 5 // In basso al Centro (Bottom-Center)
#define ANCHOR_LC 6 // Al centro a Sinistra (Left-Center)
#define ANCHOR_RC 7 // Al centro a Destra (Right-Center)
#define ANCHOR_CC 8 // Al Centro Esatto (Center-Center)

// --- COSTANTI DI DIMENSIONE ---
#define SIZE_FILL 0 // Dichiara all'oggetto di riempire tutto lo spazio disponibile

// Definiamo i tipi interi a grandezza fissa per il nostro OS a 32-bit
typedef unsigned int uint32_t;

void gui_image(int x, int y, int w, int h, uint32_t* pixel_array);
void save_file(const char* filename, const char* data, uint32_t size);
void load_file(const char* filename, char* dest_buffer, uint32_t* out_size);

// *** AGGIORNATE CON L'ANCORAGGIO ***
void gui_textbox(int anchor, int x, int y, int w, int h, uint32_t bg, int max_chars, const char* initial_text);


// --- AREA DI TESTO MULTILINEA ---
#define TEXTAREA_MAX      8192   // caratteri per area
#define TEXTAREA_EDIT     0      // modificabile
#define TEXTAREA_READONLY 1      // sola lettura: si sfoglia ma non si scrive
void gui_textarea(int anchor, int x, int y, int w, int h, uint32_t bg, int idx, int readonly);
void gui_textarea_set(int idx, const char* text);
void gui_textarea_append(int idx, const char* text);
void gui_textarea_get(int idx, char* dest, int max);


// --- IL ROBOTTINO (accesso all'intelligenza artificiale) ---
#define ROBOT_ASSENTE 0   // non e' sul vassoio di questa finestra
#define ROBOT_PRONTO  1   // presente e libero
#define ROBOT_PENSA   2   // sta elaborando una risposta
#define ROBOT_ERRORE  3
int robot_status(void);
int robot_ask(const char* prompt, int answer_area);

// --- DISPLAY DI VETRO ---
// Etichetta di sola lettura su una lastra di vetro colorato. Testo allineato a
// destra; se il display e' alto almeno 48 pixel le cifre sono ingrandite.
void gui_display(int anchor, int x, int y, int w, int h, uint32_t tint, uint32_t text_color, const char* text);

void gui_text(int anchor, int x, int y, const char* text, uint32_t color);
void gui_text_bold(int anchor, int x, int y, const char* text, uint32_t color);

void gui_get_text(int element_idx, char* dest_buffer);
char get_key();

void get_window_size(int* w, int* h);
void get_mouse_ext(int* x, int* y, int* clicked, int* win_id); // API Multi-Window
void get_mouse(int* x, int* y, int* clicked); // Mantiene vecchie app compatibili
void gui_clear();


void gui_button(int anchor, int x, int y, int w, int h, uint32_t bg, uint32_t fg, const char* text);


// Il motore degli eventi globale
void gui_poll_events();

// OGGETTO BOTTONE: Supporta Ancora e Callback!
void gui_action_button(int anchor, int x, int y, int w, int h, uint32_t bg, uint32_t fg, const char* text, void (*callback)());

// OGGETTO PANNELLO/WIDGET: Contenitore statico o Sfondo
void gui_panel(int anchor, int x, int y, int w, int h, uint32_t bg);

// impaginazione per documenti grandi:
void gui_long_text(int x, int y, const char* text, uint32_t color);

// --- GESTIONE MULTI-WINDOW ---
// Crea una nuova finestra pop-up e restituisce il suo ID (Handle)
int gui_spawn_window(const char* title, int w, int h);

// Imposta la dimensione iniziale e di base personalizzata della finestra corrente
void gui_set_window_size(int w, int h);

// Imposta la dimensione minima sotto cui l'utente non puo' ridimensionare la finestra
void gui_set_window_min_size(int w, int h);

int gui_is_window_open(int win_id);

// Cambia la "tela" su cui le funzioni gui_* andranno a dipingere (0 = Finestra Madre)
void gui_set_context(int win_id);

// Wrapper per la Syscall 1 (Stampa stringa pura)
void print(const char* str);

// La nostra magica printf! Supporta %d (numeri), %s (stringhe) e %c (caratteri)
void printf(const char* format, ...);

// Funzione di utilità per convertire numeri in stringhe
void itoa(int num, char* str, int base);

// Wrapper per la Syscall 6 (Pausa/Yield)
void yield();

// Sostituisce yield() per le App con Interfaccia Grafica
void gui_yield();



// --- STANDARD C FILE I/O ---

// Costanti per il posizionamento del cursore (fseek)
#define SEEK_SET 0 // Partendo dall'inizio del file
#define SEEK_CUR 1 // Partendo dalla posizione attuale
#define SEEK_END 2 // Partendo dalla fine del file

// La struttura che rappresenta un file aperto in RAM
typedef struct {
    char* buffer;       // Puntatore ai dati in RAM
    unsigned int size;  // Dimensione totale del file in byte
    unsigned int cursor;// Posizione attuale di lettura/scrittura
    int mode;           // 1 = Lettura ('r'), 2 = Scrittura ('w')
    char filename[64];  // Nome del file (ci servirà al momento della chiusura)
} FILE;

// Prototipi standard
FILE* fopen(const char* filename, const char* mode);
void fclose(FILE* stream);
unsigned int fread(void* ptr, unsigned int size, unsigned int count, FILE* stream);
unsigned int fwrite(const void* ptr, unsigned int size, unsigned int count, FILE* stream);
int fseek(FILE* stream, int offset, int whence);
int ftell(FILE* stream); 

// Comandi del Terminale
void clear();
void read_string(char* buf, int max_len);

// --- AUDIO API ---
void audio_play(const char* path);
void audio_stop();


// --- TCC WRAPPERS (Mock) ---
int sprintf(char *str, const char *format, ...);
int snprintf(char *str, size_t size, const char *format, ...);
int fprintf(FILE *stream, const char *format, ...);
int fflush(FILE *stream);
int fputs(const char *str, FILE *stream);
int fputc(int chara, FILE *stream);


// Flussi standard console per TCC
extern FILE *stdout;
extern FILE *stderr;
extern FILE *stdin;

// Altri Mock variadici e I/O
int vfprintf(FILE *stream, const char *format, __builtin_va_list arg);
int vsnprintf(char *str, size_t size, const char *format, __builtin_va_list arg);
int sscanf(const char *str, const char *format, ...);
int remove(const char *pathname);

// --- MENU DELLA BARRA SUPERIORE ---
#define MENU_MAX_ITEMS 8
typedef struct { const char* label; void (*cb)(void); } menu_item_t;
void gui_menu(const char* title, menu_item_t* items, int count);

#endif