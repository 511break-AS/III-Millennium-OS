// Third-Millennium Operating System - antem_stdio
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


// antem_libc/stdio.c
// Implementazione della Standard C Library per III-M OS

#include "stdio.h"
#include "stdlib.h"

// Il contesto grafico attivo (0 = Finestra Madre assegnata dal Kernel)
int current_gui_window = 0;

// Definizioni per leggere gli argomenti variabili (...)
typedef __builtin_va_list va_list;
#define va_start(v,l) __builtin_va_start(v,l)
#define va_end(v)     __builtin_va_end(v)
#define va_arg(v,l)   __builtin_va_arg(v,l)


// =====================================================================
// C RUNTIME BOOTSTRAPPER (crt0)
// Questo codice viene piazzato dal linker ESATTAMENTE a 0x40000010.
// È il vero punto di atterraggio del Kernel, e chiama in automatico il main!
// =====================================================================

// Dichiariamo che da qualche parte nel programma c'è una funzione main
extern void main();
extern void exit(int status);

__attribute__((section(".entry"), naked)) void _start() {
    asm volatile(
        "mov $0x403FFFF0, %eax \n" // Magia OS: Lo Stack parte dalla fine dei 4MB virtuali!
        "mov %eax, %esp \n"
        "call main \n"
        "push $0 \n"               // Se il main finisce, passiamo 0 a exit
        "call exit \n"
    );
}



// --- BUFFER GLOBALE ---
// Spostandolo qui, il Linker gli assegnerà un indirizzo assoluto perfetto in .bss
char printf_buffer[256];


// --- SYSCALL WRAPPERS ---
void print(const char* str) {
    asm volatile ("int $0x80" : : "a"(1), "b"((unsigned int)str));
}


// ==========================================
// ANTEM UI FRAMEWORK (Eventi e Oggetti Smart)
// ==========================================
int antem_ui_mx = -1, antem_ui_my = -1, antem_ui_mclick = 0, antem_ui_mwin = -1, antem_ui_prev_click = 0;
int antem_ui_needs_redraw = 0; // Segnala se la UI ha cambiato stato
int antem_menu_idx = 0;       // slot del prossimo gui_menu() del fotogramma
int antem_menu_action = -1;   // voce di menu scelta, letta una volta per giro

void yield() {
    asm volatile ("int $0x80" : : "a"(6));
}

// Sostituto intelligente di yield()
void gui_yield() {
    if (antem_ui_needs_redraw) {
        antem_ui_needs_redraw = 0;
        // Non andiamo in sleep (Syscall 6). Lasciamo che il ciclo ricominci all'istante!
    } else {
        yield(); // Riposo standard
    }
}



void gui_poll_events() {
    antem_ui_prev_click = antem_ui_mclick; // Salviamo lo storico
    get_mouse_ext(&antem_ui_mx, &antem_ui_my, &antem_ui_mclick, &antem_ui_mwin);

    // Voce di menu scelta dall'utente, se ce n'e' una in attesa
    asm volatile ("int $0x80" : : "a"(18), "b"(current_gui_window),
                  "c"((unsigned int)&antem_menu_action));
}

// Controlla se una finestra è ancora aperta
int gui_is_window_open(int win_id) {
    int is_active = 0;
    asm volatile ("int $0x80" : : "a"(15), "b"(win_id), "c"((unsigned int)&is_active));
    return is_active;
}


// Pulisce lo schermo del terminale (Syscall 2)
void clear() {
    asm volatile ("int $0x80" : : "a"(2));
}

// Chiede al Kernel di leggere l'input da tastiera (Syscall 3)
void read_string(char* buf, int max_len) {
    asm volatile ("int $0x80" : : "a"(3), "b"((unsigned int)buf), "c"(max_len));
}

// --- CONVERSIONE NUMERI (itoa) ---
void itoa(int num, char* str, int base) {
    int i = 0;
    int is_negative = 0;

    if (num == 0) {
        str[i++] = '0';
        str[i] = '\0';
        return;
    }

    if (num < 0 && base == 10) {
        is_negative = 1;
        num = -num;
    }

    while (num != 0) {
        int rem = num % base;
        str[i++] = (rem > 9) ? (rem - 10) + 'A' : rem + '0';
        num = num / base;
    }

    if (is_negative) str[i++] = '-';
    str[i] = '\0';

    // Ribaltiamo la stringa
    int start = 0;
    int end = i - 1;
    while (start < end) {
        char temp = str[start];
        str[start] = str[end];
        str[end] = temp;
        start++;
        end--;
    }
}

// --- LA PRINTF ---
void printf(const char* format, ...) {
    int buf_idx = 0;
    
    // Puliamo il buffer globale prima di usarlo
    for(int k=0; k<256; k++) printf_buffer[k] = '\0';
    
    va_list args;
    va_start(args, format);

    for (int i = 0; format[i] != '\0' && buf_idx < 254; i++) {
        if (format[i] == '%') {
            i++; 
            
            if (format[i] == 'd') { 
                int num = va_arg(args, int);
                char num_str[32];
                itoa(num, num_str, 10);
                for (int j = 0; num_str[j] != '\0' && buf_idx < 254; j++) {
                    printf_buffer[buf_idx++] = num_str[j];
                }
            } 
            else if (format[i] == 's') { 
                char* str = va_arg(args, char*);
                if (!str) str = "(null)"; 
                for (int j = 0; str[j] != '\0' && buf_idx < 254; j++) {
                    printf_buffer[buf_idx++] = str[j];
                }
            }
            else if (format[i] == 'c') { 
                char c = (char)va_arg(args, int);
                printf_buffer[buf_idx++] = c;
            }
            else if (format[i] == '%') { 
                printf_buffer[buf_idx++] = '%';
            }
        } else {
            printf_buffer[buf_idx++] = format[i];
        }
    }
    
    printf_buffer[buf_idx] = '\0';
    va_end(args);

    print(printf_buffer);
}


// ==========================================
// IMPLEMENTAZIONE DELLE NUOVE API GRAFICHE
// ==========================================

void gui_set_context(int win_id) {
    current_gui_window = win_id;
}

int gui_spawn_window(const char* title, int w, int h) {
    int new_win_id = -1;
    // Compattiamo Larghezza e Altezza in un solo numero a 32-bit per usare un registro in meno
    uint32_t size_packed = (w & 0xFFFF) | ((h & 0xFFFF) << 16);
    asm volatile ("int $0x80" : : "a"(14), "b"((unsigned int)title), "c"(size_packed), "d"((unsigned int)&new_win_id));
    return new_win_id;
}

void gui_set_window_size(int w, int h) {
    // Comunica la risoluzione al Kernel applicandola al contesto grafico attualmente attivo
    asm volatile ("int $0x80" : : "a"(16), "b"(w), "c"(h), "d"(current_gui_window));
}

void gui_set_window_min_size(int w, int h) {
    // Misura sotto la quale l'utente non puo' ridimensionare la finestra
    asm volatile ("int $0x80" : : "a"(19), "b"(w), "c"(h), "d"(current_gui_window));
}

void save_file(const char* filename, const char* data, uint32_t size) {
    asm volatile ("int $0x80" : : "a"(4), "b"((unsigned int)filename), "c"((unsigned int)data), "d"(size));
}



// ==========================================
// NUOVA API: CARICA FILE DAL DISCO ALLA RAM
// ==========================================
void load_file(const char* filename, char* dest_buffer, uint32_t* out_size) {
    asm volatile ("int $0x80" : : "a"(10), "b"((unsigned int)filename), "c"((unsigned int)dest_buffer), "d"((unsigned int)out_size));
}

char get_key() {
    char k = 0;
    asm volatile ("int $0x80" : : "a"(8), "b"((unsigned int)&k));
    return k;
}

// Struttura aggiornata a 16 Byte
typedef struct { int x, y, clicked, win_id; } mouse_state_ext_t;

void get_mouse(int* x, int* y, int* clicked) {
    mouse_state_ext_t m = {-1, -1, 0, -1};
    asm volatile ("int $0x80" : : "a"(5), "b"((unsigned int)&m));
    *x = m.x; *y = m.y; *clicked = m.clicked;
}

void get_mouse_ext(int* x, int* y, int* clicked, int* win_id) {
    mouse_state_ext_t m = {-1, -1, 0, -1};
    asm volatile ("int $0x80" : : "a"(5), "b"((unsigned int)&m));
    *x = m.x; *y = m.y; *clicked = m.clicked; *win_id = m.win_id;
}

void get_window_size(int* w, int* h) {
    asm volatile ("int $0x80" : : "a"(9), "b"((unsigned int)w), "c"((unsigned int)h), "d"(current_gui_window));
}

// Legge il testo attualmente contenuto in un elemento GUI (es. Textbox)
void gui_get_text(int element_idx, char* dest_buffer) {
    asm volatile ("int $0x80" : : "a"(11), "b"(element_idx), "c"((unsigned int)dest_buffer), "d"(current_gui_window));
}

typedef struct {
    int type; int x, y, w, h; unsigned int c1, c2; char text[256]; 
} gui_el_t;



void gui_clear() {
    antem_menu_idx = 0;
    gui_el_t el = {0};
    asm volatile ("int $0x80" : : "a"(7), "b"((unsigned int)&el), "c"(current_gui_window));
}

// ========================================================
// MOTORE MATEMATICO UNIVERSALE PER GLI ANCORAGGI A 9 PUNTI
// ========================================================
void apply_anchor_math(int anchor, int win_w, int win_h, int w, int h, int* x, int* y, int* flag_w, int* flag_h) {
    int area_w = win_w - 8;
    int area_h = win_h - 34;

    if (anchor == ANCHOR_TR) { 
        *x = area_w - w - *x; *flag_w = 0x0100; 
    } 
    else if (anchor == ANCHOR_BL) { 
        *y = area_h - h - *y; *flag_h = 0x0200; 
    } 
    else if (anchor == ANCHOR_BR) { 
        *x = area_w - w - *x; *y = area_h - h - *y; 
        *flag_w = 0x0100; *flag_h = 0x0200; 
    } 
    else if (anchor == ANCHOR_TC) {
        *x = (area_w / 2) - (w / 2) + *x; 
        *flag_w = 0x0100;
    } 
    else if (anchor == ANCHOR_BC) {
        *x = (area_w / 2) - (w / 2) + *x; *y = area_h - h - *y; 
        *flag_w = 0x0100; *flag_h = 0x0200;
    } 
    else if (anchor == ANCHOR_LC) {
        *y = (area_h / 2) - (h / 2) + *y; 
        *flag_h = 0x0200;
    } 
    else if (anchor == ANCHOR_RC) {
        *x = area_w - w - *x; *y = (area_h / 2) - (h / 2) + *y; 
        *flag_w = 0x0100; *flag_h = 0x0200;
    } 
    else if (anchor == ANCHOR_CC) {
        *x = (area_w / 2) - (w / 2) + *x; *y = (area_h / 2) - (h / 2) + *y; 
        *flag_w = 0x0100; *flag_h = 0x0200;
    }
}




// ========================================================================
// MISURAZIONE DEL FONT PROPORZIONALE IN USER SPACE
// ========================================================================
// Tabella delle larghezze grezze dei caratteri ASCII (0-127) estratta dal Kernel
static const unsigned char stdio_font_w[128] = {
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0, // 0-15
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0, // 16-31
    4, 2, 4, 6, 5, 6, 6, 2, 3, 3, 5, 5, 2, 4, 2, 4, // 32-47  (spazio, !, ", #, ...)
    5, 4, 5, 5, 5, 5, 5, 5, 5, 5, 2, 2, 4, 5, 4, 5, // 48-63  (0-9, :, ;, ...)
    7, 6, 6, 6, 6, 5, 5, 6, 6, 3, 5, 6, 5, 7, 6, 6, // 64-79  (@, A-O)
    6, 6, 6, 6, 5, 6, 6, 7, 6, 6, 6, 3, 4, 3, 5, 5, // 80-95  (P-Z, ...)
    3, 5, 5, 5, 5, 5, 4, 5, 5, 2, 3, 5, 2, 7, 5, 5, // 96-111 (`, a-o)
    5, 5, 4, 5, 4, 5, 5, 7, 5, 5, 5, 4, 2, 4, 6, 0  // 112-127 (p-z, ...)
};

static int stdio_get_str_w(const char* str) {
    int w = 0;
    while (*str) {
        unsigned char uc = (unsigned char)*str;
        if (uc < 128) {
            if (uc == ' ') w += 5; // Spazio normale
            else w += stdio_font_w[uc] + 1; // +1px di respiro naturale
        } else w += 8;
        str++;
    }
    return w;
}

static int stdio_get_str_w_bold(const char* str) {
    int w = 0;
    while (*str) {
        unsigned char uc = (unsigned char)*str;
        if (uc < 128) {
            if (uc == ' ') w += 6; // Spazio grassetto allargato
            else w += stdio_font_w[uc] + 2; // +1 respiro, +1 spessore grassetto
        } else w += 9;
        str++;
    }
    return w;
}

// ORA LE FUNZIONI DELLA GUI USANO IL CALCOLO REALE MILLIMETRICO
void gui_text(int anchor, int x, int y, const char* text, unsigned int color) {
    int win_w = 0, win_h = 0; get_window_size(&win_w, &win_h);
    int flag_w = 0, flag_h = 0;
    
    // Calcoliamo la larghezza in pixel REALI anziché approssimare a 7px
    int actual_w = stdio_get_str_w(text); 
    
    apply_anchor_math(anchor, win_w, win_h, actual_w, 16, &x, &y, &flag_w, &flag_h);
    
    // Azzerato: i campi non usati venivano spediti al kernel con spazzatura
    // di stack, e il ridimensionamento automatico li leggeva come dimensioni.
    gui_el_t el = {0};
    el.type = 3 | flag_w | flag_h;
    el.x = x; el.y = y; el.w = actual_w; el.h = 16; el.c1 = color;
    int i=0; while(text[i] && i<255) { el.text[i] = text[i]; i++; } el.text[i] = '\0';

    

    asm volatile ("int $0x80" : : "a"(7), "b"((unsigned int)&el), "c"(current_gui_window));
}

void gui_text_bold(int anchor, int x, int y, const char* text, uint32_t color) {
    int win_w = 0, win_h = 0; get_window_size(&win_w, &win_h);
    int flag_w = 0, flag_h = 0;
    
    // Calcoliamo la larghezza in pixel REALI per il font Grassetto
    int actual_w = stdio_get_str_w_bold(text); 
    
    apply_anchor_math(anchor, win_w, win_h, actual_w, 16, &x, &y, &flag_w, &flag_h);
    
    // Azzerato: i campi non usati venivano spediti al kernel con spazzatura
    // di stack, e il ridimensionamento automatico li leggeva come dimensioni.
    gui_el_t el = {0};
    el.type = 3 | 0x0400 | flag_w | flag_h;
    el.x = x; el.y = y; el.w = actual_w; el.h = 16; el.c1 = color;
    int i=0; while(text[i] && i<255) { el.text[i] = text[i]; i++; } el.text[i] = '\0';
    asm volatile ("int $0x80" : : "a"(7), "b"((unsigned int)&el), "c"(current_gui_window));
}




void gui_textbox(int anchor, int x, int y, int w, int h, unsigned int bg, int max_chars, const char* initial_text) {
    int win_w = 0, win_h = 0; get_window_size(&win_w, &win_h);
    int flag_w = 0, flag_h = 0;
    apply_anchor_math(anchor, win_w, win_h, w, h, &x, &y, &flag_w, &flag_h);

    gui_el_t el; 
    el.type = 4 | flag_w | flag_h; 
    el.x = x; el.y = y; el.w = w; el.h = h; 
    el.c1 = bg; el.c2 = (unsigned int)max_chars; 
    int i=0; while(initial_text[i] && i<255) { el.text[i] = initial_text[i]; i++; } el.text[i] = '\0';
    asm volatile ("int $0x80" : : "a"(7), "b"((unsigned int)&el), "c"(current_gui_window));
}
// ==========================================================
// AREA DI TESTO MULTILINEA
// Il testo vive nel kernel, non nell'elemento: l'app dichiara l'area a ogni
// fotogramma con gui_textarea (solo posizione e aspetto) e ne scrive o legge
// il contenuto solo quando le serve.
// ==========================================================
void gui_textarea(int anchor, int x, int y, int w, int h, unsigned int bg, int idx, int readonly) {
    int win_w = 0, win_h = 0; get_window_size(&win_w, &win_h);
    int flag_w = 0, flag_h = 0;
    apply_anchor_math(anchor, win_w, win_h, w, h, &x, &y, &flag_w, &flag_h);

    gui_el_t el = {0};
    el.type = 6 | flag_w | flag_h;
    el.x = x; el.y = y; el.w = w; el.h = h;
    el.c1 = bg;
    el.c2 = (unsigned int)(idx & 0xFF) | (readonly ? 0x100u : 0u);
    asm volatile ("int $0x80" : : "a"(7), "b"((unsigned int)&el), "c"(current_gui_window) : "memory");
}

// Sostituisce tutto il testo dell'area. Non va chiamata a ogni fotogramma:
// riporterebbe ogni volta il cursore all'inizio.
void gui_textarea_set(int idx, const char* text) {
    asm volatile ("int $0x80" : : "a"(20), "b"(idx), "c"((unsigned int)text), "d"(current_gui_window) : "memory");
}

// Aggiunge testo in fondo e porta la vista all'ultima riga
void gui_textarea_append(int idx, const char* text) {
    asm volatile ("int $0x80" : : "a"(22), "b"(idx), "c"((unsigned int)text), "d"(current_gui_window) : "memory");
}

// Copia il testo dell'area in dest, al massimo max-1 caratteri piu' il terminatore
void gui_textarea_get(int idx, char* dest, int max) {
    unsigned int packed = (unsigned int)(idx & 0xFF) | ((unsigned int)max << 8);
    asm volatile ("int $0x80" : : "a"(21), "b"(packed), "c"((unsigned int)dest), "d"(current_gui_window) : "memory");
}

// ==========================================================
// IL ROBOTTINO: ACCESSO ALL'INTELLIGENZA ARTIFICIALE
// Funziona solo se l'utente ha trascinato il robottino sul vassoio della
// barra del titolo di questa finestra. Il controllo lo fa il kernel.
// ==========================================================

// Stato del robottino rispetto a questa finestra (vedi ROBOT_* in stdio.h)
int robot_status(void) {
    int st = 0;
    asm volatile ("int $0x80" : : "a"(23), "b"((unsigned int)&st), "c"(0), "d"(current_gui_window) : "memory");
    return st;
}

// Invia una domanda. La risposta viene scritta dal kernel, un pezzo alla
// volta, nell'area di testo indicata: l'app non deve aspettare nulla.
// Restituisce 1 se la richiesta e' stata accettata, 0 se il permesso manca.
int robot_ask(const char* prompt, int answer_area) {
    int r = answer_area;
    asm volatile ("int $0x80" : : "a"(24), "b"((unsigned int)prompt), "c"((unsigned int)&r), "d"(current_gui_window) : "memory");
    return r;
}

// ==========================================================
// DISPLAY DI VETRO
// Etichetta di sola lettura su una lastra di vetro colorato, pensata per
// calcolatrici, contatori e orologi. Il testo e' allineato a destra; se il
// display e' alto almeno 48 pixel le cifre vengono ingrandite.
// tint = colore del vetro, text_color = colore del testo.
// ==========================================================
void gui_display(int anchor, int x, int y, int w, int h, unsigned int tint, unsigned int text_color, const char* text) {
    int win_w = 0, win_h = 0; get_window_size(&win_w, &win_h);
    int flag_w = 0, flag_h = 0;
    apply_anchor_math(anchor, win_w, win_h, w, h, &x, &y, &flag_w, &flag_h);

    gui_el_t el = {0};
    el.type = 7 | flag_w | flag_h;
    el.x = x; el.y = y; el.w = w; el.h = h;
    el.c1 = tint;
    el.c2 = text_color;
    int i = 0;
    while (text && text[i] && i < 255) { el.text[i] = text[i]; i++; }
    el.text[i] = '\0';
    asm volatile ("int $0x80" : : "a"(7), "b"((unsigned int)&el), "c"(current_gui_window));
}

void gui_button(int anchor, int x, int y, int w, int h, unsigned int bg, unsigned int fg, const char* text) {
    int win_w = 0, win_h = 0; get_window_size(&win_w, &win_h);
    int flag_w = 0, flag_h = 0;
    apply_anchor_math(anchor, win_w, win_h, w, h, &x, &y, &flag_w, &flag_h);

    gui_el_t el; el.type = 2 | flag_w | flag_h; el.x = x; el.y = y; el.w = w; el.h = h; el.c1 = bg; el.c2 = fg;
    int i=0; while(text[i] && i<255) { el.text[i] = text[i]; i++; } el.text[i] = '\0';
    asm volatile ("int $0x80" : : "a"(7), "b"((unsigned int)&el), "c"(current_gui_window));
}

// ==========================================================
void gui_long_text(int x, int y, const char* text, unsigned int color) {
    int cx = x; int cy = y; int i = 0;
    while(text[i] != '\0') {
        if (text[i] == '\n') { cx = x; cy += 16; i++; continue; }
        char chunk[16]; int c = 0;
        while(c < 15 && text[i] != '\0' && text[i] != '\n') chunk[c++] = text[i++];
        chunk[c] = '\0';
        gui_text(ANCHOR_TL, cx, cy, chunk, color); // Long text resta assoluto top-left
        cx += c * 8; 
    }
}

// ==========================================================
void gui_action_button(int anchor, int x, int y, int w, int h, uint32_t bg, uint32_t fg, const char* text, void (*callback)()) {
    int win_w = 0, win_h = 0; get_window_size(&win_w, &win_h);
    int flag_w = 0, flag_h = 0;
    int abs_x = x; int abs_y = y;
    
    apply_anchor_math(anchor, win_w, win_h, w, h, &abs_x, &abs_y, &flag_w, &flag_h);

    gui_el_t el; el.type = 2 | flag_w | flag_h; 
    el.x = abs_x; el.y = abs_y; el.w = w; el.h = h; el.c1 = bg; el.c2 = fg;
    int i=0; while(text[i] && i<255) { el.text[i] = text[i]; i++; } el.text[i] = '\0';
    asm volatile ("int $0x80" : : "a"(7), "b"((unsigned int)&el), "c"(current_gui_window));

    if (antem_ui_mwin == current_gui_window && antem_ui_mclick == 0 && antem_ui_prev_click == 1) {
        if (antem_ui_mx >= abs_x && antem_ui_mx <= abs_x + w && antem_ui_my >= abs_y && antem_ui_my <= abs_y + h) {
            if (callback != 0) callback();
            antem_ui_needs_redraw = 1; 
        }
    }
}

void gui_panel(int anchor, int x, int y, int w, int h, uint32_t bg) {
    int win_w = 0, win_h = 0; get_window_size(&win_w, &win_h);
    int area_w = win_w - 8; int area_h = win_h - 34;

    int abs_x = x; int abs_y = y; int abs_w = w; int abs_h = h;
    if (w == SIZE_FILL) abs_w = area_w - (x * 2); 
    if (h == SIZE_FILL) abs_h = area_h - (y * 2);

    int flag_w = 0, flag_h = 0;
    if (w == SIZE_FILL) flag_w = 0x0100;
    if (h == SIZE_FILL) flag_h = 0x0200;

    apply_anchor_math(anchor, win_w, win_h, abs_w, abs_h, &abs_x, &abs_y, &flag_w, &flag_h);

    gui_el_t el; el.type = 1 | flag_w | flag_h; 
    el.x = abs_x; el.y = abs_y; el.w = abs_w; el.h = abs_h; el.c1 = bg; el.c2 = 0; el.text[0] = '\0';
    asm volatile ("int $0x80" : : "a"(7), "b"((unsigned int)&el), "c"(current_gui_window));
}




// =====================================================================
// IMPLEMENTAZIONE STANDARD C FILE I/O (Wrapper per Ante-M OS)
// =====================================================================

FILE* fopen(const char* filename, const char* mode) {
    FILE* f = (FILE*)malloc(sizeof(FILE));
    if (!f) return 0; 
    
    // Allochiamo 300 KB di RAM per ospitare file ed eseguibili
    f->buffer = (char*)malloc(300000); 
    if (!f->buffer) { free(f); return 0; }
    
    f->cursor = 0;
    int n = 0; while(filename[n] != '\0' && n < 63) { f->filename[n] = filename[n]; n++; }
    f->filename[n] = '\0';
    
    if (mode[0] == 'r') {
        f->mode = 1; 
        uint32_t actual_size = 0;
        load_file(filename, f->buffer, &actual_size);
        if (actual_size == 0) {
            free(f->buffer);
            free(f);
            return 0; // Ora TCC saprà che il file manca e non crasherà!
        }
        f->size = actual_size; 
    }
    else if (mode[0] == 'w') {
        f->mode = 2; 
        f->size = 0;
        // FIX: Aggiornato al nuovo limite di 300 KB
        for(int i = 0; i < 300000; i++) f->buffer[i] = 0; 
    }
    else {
        free(f->buffer); free(f); return 0; 
    }
    
    return f;
}

void fclose(FILE* stream) {
    if (!stream) return;
    
    if (stream->mode == 2) {
        save_file(stream->filename, stream->buffer, stream->size);
    }
    
    free(stream->buffer);
    free(stream);
}

unsigned int fread(void* ptr, unsigned int size, unsigned int count, FILE* stream) {
    if (!stream || stream->mode != 1) return 0;
    
    unsigned int total_bytes = size * count;
    unsigned int bytes_read = 0;
    char* dest = (char*)ptr;
    
    // Copiamo byte per byte dalla "Sala d'attesa" in RAM al programma
    while (bytes_read < total_bytes && stream->cursor < stream->size) {
        dest[bytes_read] = stream->buffer[stream->cursor];
        bytes_read++;
        stream->cursor++;
    }
    
    return bytes_read / size; // Ritorna quanti elementi interi abbiamo letto
}

unsigned int fwrite(const void* ptr, unsigned int size, unsigned int count, FILE* stream) {
    if (!stream || stream->mode != 2) return 0;
    
    unsigned int total_bytes = size * count;
    unsigned int bytes_written = 0;
    const char* src = (const char*)ptr;
    
    // FIX: Ora possiamo scrivere fino a 300 KB!
    while (bytes_written < total_bytes && stream->cursor < 300000) {
        stream->buffer[stream->cursor] = src[bytes_written];
        bytes_written++;
        stream->cursor++;
        // Se stiamo scrivendo oltre la vecchia fine del file, la sua dimensione totale cresce!
        if (stream->cursor > stream->size) stream->size = stream->cursor;
    }
    
    if (stream->size < 300000) stream->buffer[stream->size] = '\0';
    
    return bytes_written / size;
}

// Sposta il cursore di lettura/scrittura all'interno del file
int fseek(FILE* stream, int offset, int whence) {
    if (!stream) return -1;
    
    int new_pos = 0;
    if (whence == SEEK_SET) {
        new_pos = offset; // Offset esatto dall'inizio del file
    } else if (whence == SEEK_CUR) {
        new_pos = stream->cursor + offset; // Relativo a dove siamo ora
    } else if (whence == SEEK_END) {
        new_pos = stream->size + offset; // Relativo alla fine (spesso offset è negativo qui)
    } else {
        return -1; // Parametro non valido
    }
    
    // Sicurezza: non andiamo in negativo o oltre il buffer allocato
    if (new_pos < 0 || new_pos >= 300000) return -1;
    
    stream->cursor = new_pos;
    return 0; // 0 significa Successo per fseek
}

// Rivela la posizione attuale del cursore
int ftell(FILE* stream) {
    if (!stream) return -1;
    return stream->cursor;
}


void gui_image(int x, int y, int w, int h, uint32_t* pixel_array) {
    gui_el_t el; 
    el.type = 5; 
    el.x = x; el.y = y; el.w = w; el.h = h; 
    el.c1 = (unsigned int)pixel_array; 
    asm volatile ("int $0x80" : : "a"(7), "b"((unsigned int)&el), "c"(current_gui_window));
}


void audio_play(const char* path) { 
    asm volatile ("int $0x80" : : "a"(12), "b"((unsigned int)path)); 
}

void audio_stop() { 
    asm volatile ("int $0x80" : : "a"(12), "b"(0)); 
}



// ==========================================================
// MENU DELLA BARRA SUPERIORE
// Stesso schema dei bottoni: l'app ridichiara i menu a ogni giro e la
// libreria richiama da sola il callback della voce scelta.
// ==========================================================


void gui_menu(const char* title, menu_item_t* items, int count) {
    if (count > MENU_MAX_ITEMS) count = MENU_MAX_ITEMS;

    struct { char title[24]; int item_count; char items[MENU_MAX_ITEMS][32]; } pkt;

    int i = 0;
    while (title[i] && i < 23) { pkt.title[i] = title[i]; i++; }
    pkt.title[i] = '\0';

    pkt.item_count = count;
    for (int k = 0; k < count; k++) {
        int c = 0;
        const char* s = items[k].label;
        while (s && s[c] && c < 31) { pkt.items[k][c] = s[c]; c++; }
        pkt.items[k][c] = '\0';
    }

    asm volatile ("int $0x80" : : "a"(17), "b"((unsigned int)&pkt),
                  "c"(current_gui_window), "d"(antem_menu_idx));

    if (antem_menu_action >= 0 && (antem_menu_action / 100) == antem_menu_idx) {
        int it = antem_menu_action % 100;
        antem_menu_action = -1;
        if (it < count && items[it].cb) {
            items[it].cb();
            antem_ui_needs_redraw = 1;
        }
    }
    antem_menu_idx++;
}

/*
// ==========================================
// TCC WRAPPERS (Funzioni fittizie per il Linker)
// ==========================================
int sprintf(char *str, const char *format, ...) { return 0; }
int snprintf(char *str, size_t size, const char *format, ...) { return 0; }
int fprintf(FILE *stream, const char *format, ...) { return 0; }
int fflush(FILE *stream) { return 0; }
int fputs(const char *str, FILE *stream) { return 0; }
int fputc(int chara, FILE *stream) { return chara; }

// --- MOCK POSIX I/O ---
// Li implementiamo qui dentro per comodità, così vengono inclusi in stdio.o
typedef int ssize_t; 
ssize_t read(int fd, void *buf, size_t count) { return 0; }
int unlink(const char *pathname) { return 0; }
int open(const char *pathname, int flags, ...) { return -1; }
void *fdopen(int fd, const char *mode) { return 0; }
int lseek(int fd, int offset, int whence) { return -1; }
char *getcwd(char *buf, size_t size) { return 0; }

// Flussi standard
FILE *stdout = NULL;
FILE *stderr = NULL;
FILE *stdin = NULL;

int vfprintf(FILE *stream, const char *format, va_list arg) { return 0; }
int vsnprintf(char *str, size_t size, const char *format, va_list arg) { return 0; }
int sscanf(const char *str, const char *format, ...) { return 0; }
int remove(const char *pathname) { return 0; }

*/