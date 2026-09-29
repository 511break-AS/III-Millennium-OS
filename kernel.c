// Third-Millennium Operating System
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



// COMPILAZIONE e AVVIO (NUOVO)
// i686-elf-as boot.S -o boot.o
// i686-elf-gcc -c kernel.c -o kernel.o -std=gnu99 -ffreestanding -O2 -Wall -Wextra
// i686-elf-gcc -c gfx.c -o gfx.o -std=gnu99 -ffreestanding -O2 -Wall -Wextra
// i686-elf-gcc -T linker.ld -o myos.bin -ffreestanding -O2 -nostdlib boot.o kernel.o gfx.o -lgcc
// qemu-system-i386 -kernel myos.bin -drive file=disk.img,format=raw,index=0,media=disk -m 512M -device ac97 -netdev user,id=net0 -device rtl8139,netdev=net0 -object filter-dump,id=cap0,netdev=net0,file=rete.pcap

// MODO PIU' RAPIDO PER COMPILARE E AVVIARE
// make
// make run



#include <stdint.h>
#include <stddef.h>
#include "gfx.h"

// ==========================================
// FORWARD DECLARATIONS (Prototipi di base)
// ==========================================
void print_term(const char* msg);
void* kmalloc(size_t size);
void kfree(void* ptr);
// --- FIRME PER IL DRIVER AUDIO ---
void* kmalloc_dma(size_t size);
int ext2_read_file(const char* full_path, char* dest_buffer, int max_size, uint32_t* out_file_size);
void kfree_dma(void* ptr);
void sleep(uint32_t ms);

// --- VARIABILI GLOBALI DI STATO ---
volatile uint32_t system_ticks = 0;
volatile int refresh_requested = 0;


// DEFINIZIONI COLORI STANDARD
#define COLOR_SKY_BLUE 0xFFC0C0C0  // C'è scritto skyblue ma in realtà è grigio. (c'è nebbia al nord!)



// --- VARIABILI GLOBALI PER LA BOOTSCREEN ---
char* bootscreen_logo_raw = 0;  // L'immagine grezza caricata dal disco
uint32_t bootscreen_logo_w = 0; // Larghezza
uint32_t bootscreen_logo_h = 0; // Altezza
uint32_t bootscreen_pixel_offset = 0; // Offset dei pixel


// ==========================================
// MMU E MEMORIA VIRTUALE (PAGING)
// ==========================================
uint32_t page_directory[1024] __attribute__((aligned(4096)));


void init_paging() {
    // 1. Abilitiamo le pagine (4MB) per l'Identity Mapping del Kernel (PSE)
    uint32_t cr4; asm volatile("mov %%cr4, %0" : "=r"(cr4));
    cr4 |= 0x00000010; asm volatile("mov %0, %%cr4" :: "r"(cr4));

    // 2. Identity Map: I primi 4 GigaByte Fisici restano accessibili normalmente dal Kernel
    for(uint32_t i = 0; i < 1024; i++) {
        page_directory[i] = (i * 0x400000U) | 0x83; // Present, R/W, 4MB Page
    }

    // 3. Carichiamo la Directory e accendiamo la Memoria Virtuale!
    asm volatile("mov %0, %%cr3":: "r"(page_directory));
    uint32_t cr0; asm volatile("mov %%cr0, %0": "=r"(cr0));
    cr0 |= 0x80000000; asm volatile("mov %0, %%cr0":: "r"(cr0));
}

// ==========================================
// INFORMAZIONI HARDWARE DISCO FISSO
// ==========================================
uint32_t ata_drive_capacity_sectors = 0;
uint32_t ata_drive_capacity_mb = 0;
char ata_drive_model[41];


// ==========================================
// INFORMAZIONI MOUNT EXT2 (Mappa Block Groups)
// ==========================================
uint32_t ext2_blocks_per_group = 0;
uint32_t ext2_inodes_per_group = 0;
uint32_t ext2_total_groups = 0;
uint32_t ext2_bgdt_lba = 4; // LBA della Block Group Descriptor Table


// ==========================================
// I/O HARDWARE
// ==========================================
static inline void outb(uint16_t port, uint8_t val) { asm volatile ( "outb %0, %1" : : "a"(val), "Nd"(port) ); }
static inline uint8_t inb(uint16_t port) { uint8_t ret; asm volatile ( "inb %1, %0" : "=a"(ret) : "Nd"(port) ); return ret; }
static inline void outw(uint16_t port, uint16_t val) { asm volatile ( "outw %0, %1" : : "a"(val), "Nd"(port) ); }
// Legge 16 bit (una Word) da una porta
static inline uint16_t inw(uint16_t port) {
    uint16_t ret;
    asm volatile ( "inw %1, %0" : "=a"(ret) : "Nd"(port) );
    return ret;
}

static inline void outl(uint16_t port, uint32_t val) { asm volatile ( "outl %0, %1" : : "a"(val), "Nd"(port) ); }
static inline uint32_t inl(uint16_t port) { uint32_t ret; asm volatile ( "inl %1, %0" : "=a"(ret) : "Nd"(port) ); return ret; }


// --- HOOK DEL DRIVER AUDIO GLOBALE ---
int (*sys_audio_play)(const char* path) = 0; // Inizialmente nullo

void register_audio_driver(void* play_func_ptr) {
    // Il driver ci passa la sua funzione, noi la salviamo nel Kernel
    sys_audio_play = (int (*)(const char*))play_func_ptr;
}


// --- HOOK DEL DRIVER DI RETE ---
// Deve coincidere campo per campo con net_driver_t del driver.
typedef struct {
    int  (*send)(const uint8_t* frame, uint16_t len);   // 1 = inviato
    int  (*receive)(uint8_t* dest, uint16_t max);       // byte copiati, 0 = niente
    void (*get_mac)(uint8_t* mac);
    uint16_t io;
    uint8_t  irq;
} net_driver_t;

net_driver_t* net_drv = 0;   // resta nullo finche' nessun driver si registra

void register_net_driver(void* ops) {
    net_drv = (net_driver_t*)ops;
}


// ==========================================
// KERNEL API TABLE (Per i Driver)
// ==========================================
// Questa struttura è il "dizionario" che passeremo ai driver esterni.
typedef struct {
    void (*print_term)(const char*);
    void* (*kmalloc)(size_t);
    void (*kfree)(void*);
    void (*outb)(uint16_t, uint8_t);
    uint8_t (*inb)(uint16_t);
    void (*outw)(uint16_t, uint16_t);
    uint16_t (*inw)(uint16_t);
    void* (*kmalloc_dma)(size_t);
    int (*ext2_read_file)(const char*, char*, int, uint32_t*);
    void (*kfree_dma)(void*);
    void (*register_audio)(void*);
    void (*register_net)(void*);     // SEMPRE in coda: i driver gia' compilati restano validi
} kernel_api_t;

kernel_api_t driver_api;

// Riempiamo il dizionario con gli indirizzi REALI delle funzioni del Kernel
void init_driver_api() {
    driver_api.print_term = print_term;
    driver_api.kmalloc = kmalloc;
    driver_api.kfree = kfree;
    driver_api.outb = outb;
    driver_api.inb = inb;
    driver_api.outw = outw;
    driver_api.inw = inw;
    driver_api.kmalloc_dma = kmalloc_dma;
    driver_api.ext2_read_file = ext2_read_file;
    driver_api.kfree_dma = kfree_dma;
    driver_api.register_audio = register_audio_driver;
    driver_api.register_net = register_net_driver;
}

// ==========================================
// API MOUSE (Per Syscall 5)
// ==========================================
typedef struct {
    int x;       // Posizione X relativa alla finestra dell'App
    int y;       // Posizione Y relativa alla finestra dell'App
    int clicked; // 1 se il tasto sinistro è premuto, 0 altrimenti
    int win_id;  // Diciamo all'app QUALE finestra ha ricevuto il click
} mouse_state_t;

// ==========================================
// API GRAFICA (Per Syscall 7)
// ==========================================
typedef struct {
    int type;        // 0 = Pulisci tutto, 1 = Rettangolo, 2 = Bottone 3D, 3 = Testo
    int x, y, w, h;  // Posizione e dimensioni relative alla finestra
    uint32_t color1; // Colore di sfondo (o del testo se type=3)
    uint32_t color2; // Colore del testo sul bottone
    char text[256];   // Testo
} gui_element_t;


// ==========================================
// AREA DI TESTO MULTILINEA (elemento di tipo 6)
// ==========================================
// Il testo di una textbox a riga singola viaggia dentro l'elemento, nei suoi
// 256 caratteri, e l'app lo rimanda a ogni fotogramma. Per un'area multilinea
// non basta: il testo vive in un buffer del kernel, legato alla finestra, e
// l'app lo scrive o lo legge solo quando le serve, con syscall dedicate.
#define TA_MAX 4          // aree di testo per finestra
#define TA_CAP 8192       // caratteri per area

typedef struct {
    char*    buf;         // 0 = non ancora creata
    int      len;
    int      cursor;      // posizione del cursore nel testo
    int      scroll;      // prima riga visibile, dopo l'andata a capo
    uint32_t version;     // cresce a ogni modifica: entra nella firma della memoria
} textarea_t;


// Componenti del terminale
// ==========================================
// VARIABILI GLOBALI
// ==========================================
#define TERM_MAX_ROWS 150
#define TERM_VIS_ROWS 16
#define TERM_COLS 150


// ==========================================
// VARIABILI PER LA MACCHINA A STATI (RMDIR)
// ==========================================
int pending_rmdir = 0;           // 1 se stiamo aspettando una risposta (y/n)
char pending_rmdir_path[64];     // Salva il percorso da distruggere

// ==========================================
// VARIABILI GLOBALI PROGRESS BAR E VFS LOCK
// ==========================================
int show_progress_bar = 0;
uint32_t progress_current = 0;
uint32_t progress_total = 0;

// IL LUCCHETTO DEL DISCO: 0 = Libero, 1 = Occupato
volatile int vfs_lock = 0;


// ==========================================
// WINDOW MANAGER E GRAFICA GLOBALE
// ==========================================
int mouse_x = 0;
int mouse_y = 0;
int is_clicking = 0;
int prev_clicking = 0; 

// --- MOTORE DI ANIMAZIONE ASINCRONO (Linear Interpolation) ---
volatile int anim_active = 0;
volatile int anim_x = 0, anim_y = 0, anim_w = 0, anim_h = 0;
volatile int anim_start_x = 0, anim_start_y = 0, anim_start_w = 0, anim_start_h = 0;
volatile int anim_target_x = 0, anim_target_y = 0, anim_target_w = 0, anim_target_h = 0;
volatile int anim_frames_total = 0;
volatile int anim_frames_left = 0;
volatile int taskbar_y_offset = 5;       // Offset attuale (5 = fluttuante, 0 = ancorata in basso)
volatile int taskbar_target_offset = 5;  // Offset desiderato

// ----------------------------------------------------------------------------
// GEOMETRIA VERTICALE DELLA TASKBAR — unica fonte di verita'.
// Il pannello di vetro tocca sempre il bordo inferiore dello schermo: e' la sua
// ALTEZZA a cambiare, non la posizione. Ancorata (offset 0) e' bassa e stretta,
// cosi' gli elementi centrati finiscono a ridosso del bordo; fluttuante
// (offset 5) e' piu' alta e gli elementi salgono.
// Disegno e gestione dei click devono leggere da qui, altrimenti divergono.
// ----------------------------------------------------------------------------
#define TASKBAR_CONTENT_H 26     // altezza dei bottoni e degli altri elementi

#define TASKBAR_BASE_H 30        // altezza del pannello con la barra ancorata

static int taskbar_panel_h(void)   { return TASKBAR_BASE_H + taskbar_y_offset * 2; }

// Posizione che il pannello avra' una volta ancorato. Serve a chi deve
// dimensionarsi sullo stato FINALE della barra invece che su quello corrente:
// al momento del click la barra e' ancora fluttuante e si ritira dopo, quindi
// usare taskbar_panel_top() lascerebbe una fessura per tutta l'animazione.
static int taskbar_panel_docked_top(void) { return fb_height - TASKBAR_BASE_H; }
static int taskbar_panel_top(void) { return fb_height - taskbar_panel_h(); }
static int taskbar_content_y(void) {
    return taskbar_panel_top() + (taskbar_panel_h() - TASKBAR_CONTENT_H) / 2;
}

// Gestione Larghezza Dinamica della Taskbar
volatile int taskbar_w = 220;            // Larghezza attuale animata (Parte compatta!)
volatile int taskbar_target_w = 220;     // Larghezza desiderata calcolata

#define MAX_WINDOWS 10
#define WIN_TYPE_TERMINAL 1
#define WIN_TYPE_SETTINGS 2 
#define WIN_TYPE_START_MENU 3 // Menu
#define WIN_TYPE_APP_GUI 4 // La tela vuota per le app EDXI
#define WIN_TYPE_APP_MENU 5 // Sottomenu Applicazioni

typedef struct {
    int active;       
    int type;         
    char title[32];
    char full_path[64]; // Percorso completo per la nuvoletta   
    int x, y, w, h;
    
    // LIMITI DINAMICI DEL BOUNDING BOX
    int min_w;
    int min_h;
    int base_w; // Memoria della larghezza iniziale/base richiesta dall'app
    int base_h; // Memoria dell'altezza iniziale/base richiesta dall'app
    // Minimo scelto dallo sviluppatore con gui_set_window_min_size.
    // 0 = non impostato: il pavimento resta la dimensione di apertura, come prima.
    int dev_min_w;
    int dev_min_h;
    
    // CAMPI PER IL MASSIMIZZA
    int is_maximized;
    int is_minimized;
    int prev_x, prev_y, prev_w, prev_h;

    int creation_id;
    int icon_id; // VARIABILE PER LE ICONE
    int owner_pid; // Quale Processo possiede questa finestra? (0 = Kernel)

    // RAM PRIVATA DI QUESTA FINESTRA
    char t_history[TERM_MAX_ROWS][TERM_COLS];
    char t_buffer[256];
    int t_len;
    int t_cursor;
    int t_scroll;

    // TELA GRAFICA DELL'APP
    gui_element_t ui_elements[32]; // Può contenere fino a 32 bottoni/elementi grafici
    int ui_count;
    textarea_t ta[TA_MAX];         // aree di testo multilinea della finestra
    int is_initializing; // La finestra resta nascosta durante il primo setup

    // CACHE SCROLLBAR
    int t_max_scrl;
    int t_thumb_y;
    int t_thumb_h;
    int t_head; 

    // --- MENU DELLA BARRA SUPERIORE ---
    char menu_titles[6][24];
    char menu_items[6][8][32];
    int  menu_item_count[6];
    int  menu_count;
    int  menu_action;      // (menu * 100 + voce) + 1 ; 0 = nessuna

    // --- MEMORIA DELLA FINESTRA ---
    // I pixel della finestra gia' disegnata, riusati finche' la firma dello
    // stato non cambia: cosi' una finestra ferma, o trascinata, non va
    // rigenerata a ogni fotogramma.
    uint32_t* cache;       // 0 = nessuna memoria
    int       cache_w, cache_h;
    uint32_t  cache_sig;   // firma dello stato con cui e' stata disegnata
    int       cache_ok;

} window_t;

window_t windows[MAX_WINDOWS];
// Menu a tendina attualmente aperto nella barra superiore.
int open_menu_win = -1;
int open_menu_idx = -1;
// ----------------------------------------------------------------------------
// MASCOTTE: dove abita e se e' in mano al mouse.
// mascot_host: -1 = a casa, sulla taskbar; altrimenti l'indice della finestra
// sul cui vassoio e' appoggiata.
// La posizione di casa la scrive il render della taskbar, invece di
// ricalcolarla: dipende dalla larghezza animata della barra e da quanti
// pulsanti ci sono, e duplicare quel conto sarebbe una fonte di disallineamenti.
// ----------------------------------------------------------------------------
#define MASCOT_W 13
#define MASCOT_H 21

int mascot_host = -1;
int mascot_drag = 0;
int mascot_grab_dx = 0, mascot_grab_dy = 0;
int mascot_home_x = 0, mascot_home_y = 0;
int home_tray_cx = 0, home_tray_base = 0;   // vassoio di casa, scritto dal render
int mascot_tip = 0;                          // nuvoletta "dove sono" aperta

// Angolo in alto a sinistra della mascotte quando siede sul vassoio di una finestra
static int mascot_seat_x(window_t* win) { return win->x + 8; }
static int mascot_seat_y(window_t* win) { return win->y + 3; }   // 2px sopra il vassoio

extern int global_active_win;   // definita subito piu' sotto

// Vassoio nella barra superiore, a sinistra dei comandi della finestra
// massimizzata. La mascotte ci siede sprofondata: ne affiora solo la testa.
#define PANEL_TRAY_BASE 22
#define PANEL_MASCOT_Y  8
static int panel_tray_cx(void) { return fb_width - 100; }

// Tipi di finestra che hanno un vassoio
static int window_tray_type(int i) {
    return windows[i].type != WIN_TYPE_TERMINAL
        && windows[i].type != WIN_TYPE_START_MENU
        && windows[i].type != WIN_TYPE_APP_MENU;
}

// Vassoio sulla barra del titolo: solo a finestra normale
static int window_has_tray(int i) {
    return windows[i].active && !windows[i].is_minimized && !windows[i].is_maximized
        && window_tray_type(i);
}

// Vassoio nella barra superiore: solo se quella finestra e' massimizzata
static int panel_tray_active(int i) {
    return i >= 0 && i < MAX_WINDOWS && windows[i].active && !windows[i].is_minimized
        && windows[i].is_maximized && window_tray_type(i);
}

// Dove si trova la mascotte in questo momento:
//   0 = da nessuna parte: resta assegnata alla sua finestra ma non ha un posto
//       visibile dove sedersi. Succede se la finestra e' minimizzata, oppure se
//       e' massimizzata ma un'altra app possiede la barra superiore. Non si vede
//       e non si afferra; il vassoio di casa vuoto dice dove si trova.
//   1 = a casa, sul vassoio della taskbar: SOLO quando non e' assegnata a nessuno
//   2 = sul vassoio della barra del titolo della finestra ospite
//   3 = sul vassoio della barra superiore: l'ospite e' massimizzato e ha il
//       fuoco, cioe' possiede la barra
static int mascot_where(void) {
    if (mascot_host < 0 || mascot_host >= MAX_WINDOWS || !windows[mascot_host].active)
        return 1;
    if (windows[mascot_host].is_minimized) return 0;
    if (windows[mascot_host].is_maximized)
        return (mascot_host == global_active_win && panel_tray_active(mascot_host)) ? 3 : 0;
    if (window_has_tray(mascot_host)) return 2;
    return 1;
}

// Posizione attuale, ovunque si trovi. Da invisibile e' fuori schermo, cosi'
// il test di presa non puo' scattare su un vassoio che sembra vuoto.
static void mascot_pos(int* x, int* y) {
    int where = mascot_where();
    if (where == 2) {
        *x = mascot_seat_x(&windows[mascot_host]);
        *y = mascot_seat_y(&windows[mascot_host]);
    } else if (where == 3) {
        *x = panel_tray_cx() - MASCOT_W / 2;
        *y = PANEL_MASCOT_Y;
    } else if (where == 1) {
        *x = mascot_home_x;
        *y = mascot_home_y;
    } else {
        *x = -1000;
        *y = -1000;
    }
}

// Il punto e' sopra la mascotte? Nella barra superiore conta solo la testa
// che si vede: il corpo e' sprofondato nel vassoio, e cliccando li' sotto si
// deve colpire la finestra, non lei.
static int mascot_hit(int mx, int my) {
    int x, y;
    mascot_pos(&x, &y);
    int bottom = (mascot_where() == 3) ? PANEL_TRAY_BASE : y + MASCOT_H;
    return mx >= x && mx < x + MASCOT_W && my >= y && my < bottom;
}

int window_order[MAX_WINDOWS]; 
int global_active_win = -1; // Traccia chi ha il focus (colore blu e tastiera)
int dragged_window = -1;
int resizing_window = -1; // Ricorda chi stiamo ridimensionando
// Ridimensionamento a cornice: durante il trascinamento la finestra resta della
// sua misura, e questa e' la misura proposta, che si applica al rilascio
int resize_frame_w = 0, resize_frame_h = 0;
int scrolling_window = -1; // Scrollbar trascinata
int drag_offset_x = 0;
int drag_offset_y = 0;
int scroll_drag_offset_y = 0; // Offset del click sulla scrollbar
int scroll_start_val = 0;     // Valore dello scroll al momento del click

// VARIABILI PER LA LOGICA DEI BOTTONI
int pressed_window_id = -1; // Finestra di cui stiamo premendo un bottone
int pressed_window_btn = 0; // 0=Nessuno, 1=Chiudi, 2=Massimizza, 3=Minimizza
int pressed_info_btn = 0;   // 1 = Stiamo tenendo premuto il tasto Info sulla Taskbar
int active_tooltip_window = -1; // Quale finestra sta mostrando la nuvoletta gialla?

// ==========================================
// VARIABILI PER LE TEXTBOX E IL FOCUS
// ==========================================
int focused_window_id = -1;   // Quale finestra ha il Focus della tastiera?
int focused_element_idx = -1; // Quale Textbox in quella finestra stiamo scrivendo?
int focused_cursor_pos = 0;   // Ricorda in che punto esatto della frase ci troviamo
int blink_counter = 0;        // Contatore per far lampeggiare il cursore '|'

// ==========================================
// MULTITASKING E SCHEDULER (PREPARAZIONE)
// ==========================================

#define MAX_PROCESSES 16

typedef struct {
    int active;             // 1 se il processo è vivo
    uint32_t esp;           // L'indirizzo esatto a cui era arrivato
    uint32_t stack_base;    // La memoria RAM privata (stack)
    uint32_t* page_table;   // La Page Table privata dell'App
    int linked_window;      // La finestra grafica a cui è ancorato
    int parent_window;      // La finestra "madre" che l'ha lanciato

    int waiting_for_input;
    char* input_buffer;
    int input_max;
    int input_idx;

    int is_critical_io;
} process_t;

process_t process_table[MAX_PROCESSES];
int current_pid = 0;   // 0 = Il Kernel (Desktop), 1+ = Le Applicazioni



// --- MOTORE DI CONTEXT SWITCHING E DIROTTAMENTO I/O ---
int get_active_term() {
    // Se a parlare è un'App, usa la sua finestra nativa...
    if (current_pid > 0) {
        int linked = process_table[current_pid].linked_window;
        
        // ...MA se è un'App GUI e ha un Terminale Genitore, dirotta l'I/O al genitore!
        if (windows[linked].type == WIN_TYPE_APP_GUI && process_table[current_pid].parent_window != -1) {
            return process_table[current_pid].parent_window;
        }
        
        return linked;
    }

    // Se è il Kernel, usa la finestra correntemente in cima
    for(int i=0; i<MAX_WINDOWS; i++) {
        int w = window_order[i];
        if(windows[w].active && windows[w].type == WIN_TYPE_TERMINAL) return w;
    }
    return 0;
}

// Qualsiasi riga di codice sotto questo punto che usa le parole
// "term_buffer" o "term_len", verrà automaticamente dirottata dal compilatore 
// sulla RAM privata della finestra attiva. Nessun refactoring necessario.
#define term_history (windows[get_active_term()].t_history)
#define term_buffer  (windows[get_active_term()].t_buffer)
#define term_len     (windows[get_active_term()].t_len)
#define term_cursor  (windows[get_active_term()].t_cursor)
#define term_scroll  (windows[get_active_term()].t_scroll)


// Questa funzione ora si limita a salvare la stringa GREZZA nello storico.
// L'impaginazione dinamica la farà il motore grafico in tempo reale!
void print_term(const char* msg) {
    if (msg[0] == '\0') msg = " ";
    int w = get_active_term();
    int head = windows[w].t_head;

    // 1. Scriviamo nella riga puntata da head (Niente più cicli di spostamento memorie!)
    int c = 0;
    while (msg[c] != '\0' && c < TERM_COLS - 1) {
        if (msg[c] != '\n') windows[w].t_history[head][c] = msg[c]; 
        else windows[w].t_history[head][c] = ' '; 
        c++;
    }
    windows[w].t_history[head][c] = '\0';

    // 2. Avanziamo l'indice in modo circolare
    head++;
    if (head >= TERM_MAX_ROWS) head = 0;
    windows[w].t_head = head;

    // 3. Puliamo la nuova riga (che ora è la più vecchia)
    windows[w].t_history[head][0] = '\0';
    windows[w].t_scroll = 0;
}

// Funzione per convertire un numero in una stringa di testo
void itoa(uint32_t num, char* str) {
    if (num == 0) { str[0] = '0'; str[1] = '\0'; return; }
    int i = 0;
    uint32_t temp = num;
    while (temp > 0) { temp /= 10; i++; }
    str[i] = '\0';
    while (num > 0) {
        str[--i] = (num % 10) + '0';
        num /= 10;
    }
}

void itoa_hex(uint32_t num, char* str) {
    char hex_chars[] = "0123456789ABCDEF";
    str[0] = '0'; str[1] = 'x';
    if (num == 0) { str[2] = '0'; str[3] = '\0'; return; }
    int i = 2;
    uint32_t temp = num;
    while (temp > 0) { temp >>= 4; i++; }
    str[i] = '\0';
    while (num > 0) {
        str[--i] = hex_chars[num & 0x0F];
        num >>= 4;
    }
}

// Converte una stringa Esadecimale (es: "FFC0C0C0") in un numero intero a 32-bit
uint32_t hex_to_uint32(const char* hex_str) {
    uint32_t result = 0;
    int i = 0;
    // Ignoriamo un eventuale '#' iniziale per flessibilità
    if (hex_str[0] == '#') i++; 
    
    while (hex_str[i] != '\0' && hex_str[i] != '"') {
        char c = hex_str[i];
        uint32_t val = 0;
        
        if (c >= '0' && c <= '9') val = c - '0';
        else if (c >= 'A' && c <= 'F') val = 10 + (c - 'A');
        else if (c >= 'a' && c <= 'f') val = 10 + (c - 'a');
        else break; // Carattere non valido, interrompiamo
        
        result = (result << 4) | val;
        i++;
    }
    return result;
}


char scancode_to_ascii[128] = {
    0,  27, '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=', '\b',
  '\t', 'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[', ']', '\n',
    0,  'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', ';', '\'', '`',
    0, '\\', 'z', 'x', 'c', 'v', 'b', 'n', 'm', ',', '.', '/', 0,
    '*', 0, ' ', 0
};


// ==========================================
// THEME LOADER (JSON Parser)
// ==========================================
// Cerca una "chiave" nel testo JSON e ne restituisce il valore esadecimale.
// Ritorna 0 se non la trova (usato per i fallback).
uint32_t json_get_color(const char* json_data, const char* key) {
    int data_len = 0; while(json_data[data_len]) data_len++;
    int key_len = 0; while(key[key_len]) key_len++;
    
    for (int i = 0; i < data_len - key_len; i++) {
        // Cerchiamo la chiave esatta
        int match = 1;
        for (int k = 0; k < key_len; k++) {
            if (json_data[i + k] != key[k]) { match = 0; break; }
        }
        
        if (match) {
            // Trovata! Saltiamo i ":" e gli spazi per arrivare al valore
            int val_idx = i + key_len;
            while (json_data[val_idx] == ' ' || json_data[val_idx] == ':' || json_data[val_idx] == '"') {
                val_idx++;
                if (val_idx >= data_len) return 0;
            }
            
            // Abbiamo l'inizio della stringa esadecimale, convertiamola
            return hex_to_uint32(&json_data[val_idx]);
        }
    }
    return 0; // Chiave non trovata
}



// Carica un file tema dal disco e lo applica istantaneamente
int load_theme(const char* filepath) {
    uint32_t file_size = 0;
    
    // 1. Chiediamo al disco quanto pesa il file
    if (!ext2_read_file(filepath, NULL, 0, &file_size)) return 0; // File non trovato
    if (file_size == 0 || file_size > 10000) return 0; // Vuoto o troppo grande
    
    // 2. Allochiamo la RAM per ospitarlo
    char* json_buf = (char*)kmalloc(file_size + 1);
    if (!json_buf) return 0; // RAM esaurita
    
    // 3. Leggiamo il file
    if (!ext2_read_file(filepath, json_buf, file_size, &file_size)) {
        kfree(json_buf); return 0;
    }
    json_buf[file_size] = '\0'; // Sicurezza stringa
    
    // 4. PARSER: Aggiorniamo la struttura globale attiva pescando i valori
    // Se una chiave manca nel file, manteniamo il colore attualmente attivo (Fallback sicuro)
    
    uint32_t c;
    if ((c = json_get_color(json_buf, "\"desktop_bg\""))) active_theme.desktop_bg = c;
    if ((c = json_get_color(json_buf, "\"primary_color\""))) active_theme.primary_color = c;
    if ((c = json_get_color(json_buf, "\"secondary_color\""))) active_theme.secondary_color = c;
    if ((c = json_get_color(json_buf, "\"taskbar_bg\""))) active_theme.taskbar_bg = c;
    if ((c = json_get_color(json_buf, "\"taskbar_btn_active\""))) active_theme.taskbar_btn_active = c;
    if ((c = json_get_color(json_buf, "\"taskbar_btn_idle\""))) active_theme.taskbar_btn_idle = c;
    if ((c = json_get_color(json_buf, "\"window_bg\""))) active_theme.window_bg = c;
    if ((c = json_get_color(json_buf, "\"window_close_btn\""))) active_theme.window_close_btn = c;
    if ((c = json_get_color(json_buf, "\"tooltip_bg\""))) active_theme.tooltip_bg = c;
    if ((c = json_get_color(json_buf, "\"scrollbar_track\""))) active_theme.scrollbar_track = c;
    if ((c = json_get_color(json_buf, "\"window_ctrl_btn\""))) active_theme.window_ctrl_btn = c;
    if ((c = json_get_color(json_buf, "\"titlebar_base\""))) active_theme.titlebar_base = c;
    if ((c = json_get_color(json_buf, "\"titlebar_lines\""))) active_theme.titlebar_lines = c;
    if ((c = json_get_color(json_buf, "\"window_ctrl_btn\""))) active_theme.window_ctrl_btn = c;
    if ((c = json_get_color(json_buf, "\"titlebar_base\""))) active_theme.titlebar_base = c;
    if ((c = json_get_color(json_buf, "\"titlebar_lines\""))) active_theme.titlebar_lines = c;
    if ((c = json_get_color(json_buf, "\"menu_hover_bg\""))) active_theme.menu_hover_bg = c;
    if ((c = json_get_color(json_buf, "\"menu_hover_lines\""))) active_theme.menu_hover_lines = c;
    if ((c = json_get_color(json_buf, "\"button_body\""))) active_theme.button_body = c;
    if ((c = json_get_color(json_buf, "\"button_body_pressed\""))) active_theme.button_body_pressed = c;
    if ((c = json_get_color(json_buf, "\"button_veil\""))) active_theme.button_veil = c;
    if ((c = json_get_color(json_buf, "\"button_rim\""))) active_theme.button_rim = c;
    if ((c = json_get_color(json_buf, "\"button_rays\""))) active_theme.button_rays = c;

    kfree(json_buf); // Pulizia
    
    // 5. Forza il Kernel a ridipingere l'intero schermo all'istante
    refresh_requested = 1; 
    return 1;
}

// ==========================================
// FILE SYSTEM EXT2 (Le Fondamenta)
// ==========================================
// __attribute__((packed)): dice al compilatore C 
// di non aggiungere byte vuoti di "spaziatura" nella struttura!
typedef struct {
    uint32_t s_inodes_count;      // Numero totale di Inode (quanti file e cartelle max)
    uint32_t s_blocks_count;      // Numero totale di blocchi
    uint32_t s_r_blocks_count;
    uint32_t s_free_blocks_count; // Blocchi ancora liberi
    uint32_t s_free_inodes_count; // Inode ancora liberi
    uint32_t s_first_data_block;
    uint32_t s_log_block_size;
    uint32_t s_log_frag_size;
    uint32_t s_blocks_per_group;
    uint32_t s_frags_per_group;
    uint32_t s_inodes_per_group;
    uint32_t s_mtime;
    uint32_t s_wtime;
    uint16_t s_mnt_count;
    uint16_t s_max_mnt_count;
    uint16_t s_magic;             // Deve essere 0xEF53
} __attribute__((packed)) ext2_superblock_t;

// Il Descrittore del Gruppo (ci dice dove sono le tabelle)
typedef struct {
    uint32_t bg_block_bitmap;
    uint32_t bg_inode_bitmap;
    uint32_t bg_inode_table;      // Il blocco in cui iniziano gli Inode.
    uint16_t bg_free_blocks_count;
    uint16_t bg_free_inodes_count;
    uint16_t bg_used_dirs_count;
    uint16_t bg_pad;
    uint32_t bg_reserved[3];
} __attribute__((packed)) ext2_bg_desc_t;

// L'Inode (La carta d'identità di un file o cartella)
typedef struct {
    uint16_t i_mode;        // Ci dice se è un file (0x8000) o una cartella (0x4000)
    uint16_t i_uid;
    uint32_t i_size;
    uint32_t i_atime;
    uint32_t i_ctime;
    uint32_t i_mtime;
    uint32_t i_dtime;
    uint16_t i_gid;
    uint16_t i_links_count;
    uint32_t i_blocks;
    uint32_t i_flags;
    uint32_t i_osd1;
    uint32_t i_block[15];         // L'array che contiene gli indirizzi dei dati.
} __attribute__((packed)) ext2_inode_t;

// La Directory Entry (Un singolo file dentro una cartella)
typedef struct {
    uint32_t inode;
    uint16_t rec_len;             // Quanto è lunga l'intera entry (per saltare alla prossima)
    uint8_t  name_len;            // Quanto è lungo il nome del file
    uint8_t  file_type;
    // Il nome del file inizia qui, ma ha lunghezza variabile
} __attribute__((packed)) ext2_dir_entry_t;



void init_graphics() {
    uint16_t BGA_INDEX = 0x01CE, BGA_DATA  = 0x01CF;
    outw(BGA_INDEX, 4); outw(BGA_DATA, 0);       
    outw(BGA_INDEX, 1); outw(BGA_DATA, 1024);    
    outw(BGA_INDEX, 2); outw(BGA_DATA, 768);     
    outw(BGA_INDEX, 3); outw(BGA_DATA, 32);      
    outw(BGA_INDEX, 4); outw(BGA_DATA, 0x41);    
}


// ----------------------------------------------------------------------------
// Posizione dei tre pulsanti di finestra. Quando la finestra e' massimizzata la
// sua barra del titolo scorre sotto il pannello dei menu, e i comandi migrano
// in alto a destra nel pannello stesso.
// Disegno, hover e click leggono tutti da qui: sono cinque punti nel codice, e
// se divergessero i pulsanti si disegnerebbero dove non si possono premere.
// ----------------------------------------------------------------------------
static void window_btns(window_t* win, int* close_x, int* max_x, int* min_x, int* btn_y) {
    if (win->is_maximized) {
        *close_x = fb_width - 30;
        *btn_y   = (PANEL_BAR_H - 20) / 2;
    } else {
        *close_x = win->x + win->w - 29;
        *btn_y   = win->y + 6;
    }
    *max_x = *close_x - 27;
    *min_x = *close_x - 54;
}

// Finché non cambia, la scena è identica: basta spostare la freccia.
int compute_hover_sig() {
    int sig = 1;

    // A. Pulsanti della barra del titolo (solo finestra in primo piano)
    int top = global_active_win;
    if (top >= 0 && top < MAX_WINDOWS && windows[top].active && !windows[top].is_minimized) {
        int btn_x, max_x, min_x, btn_y;
        window_btns(&windows[top], &btn_x, &max_x, &min_x, &btn_y);
        int on = (mouse_x >= min_x && mouse_x <= btn_x + 23 &&
                  mouse_y >= btn_y && mouse_y <= btn_y + 19);
        sig = sig * 31 + (on ? 2 : 1);
    }

    // B. Voci evidenziate nei menu aperti
    for (int i = 0; i < MAX_WINDOWS; i++) {
        if (!windows[i].active || windows[i].is_minimized) continue;
        if (windows[i].type != WIN_TYPE_START_MENU &&
            windows[i].type != WIN_TYPE_APP_MENU) continue;

        int band = 0;
        if (mouse_x >= windows[i].x && mouse_x < windows[i].x + windows[i].w &&
            mouse_y >= windows[i].y && mouse_y < windows[i].y + windows[i].h) {
            band = 1 + (mouse_y - windows[i].y) / 4;
        }
        sig = sig * 131 + (i + 1) * 512 + band;
    }

    return sig;
}

// Il movimento del mouse cambia davvero la scena, o basta spostare la freccia?
int last_hover_sig = 0;

// ========================================================
// MOTORE BMP INTEGRATO NEL KERNEL (Per Bootscreen)
// ========================================================

int load_bootscreen_logo() {
    char path[] = "/system/media/bootscreen.bmp";
    uint32_t file_size = 0;

    ext2_read_file(path, 0, 0, &file_size);
    if (file_size == 0) return 0;

    bootscreen_logo_raw = (char*)kmalloc(file_size);
    if (!bootscreen_logo_raw) return 0;

    ext2_read_file(path, bootscreen_logo_raw, file_size, &file_size);

    if (bootscreen_logo_raw[0] != 'B' || bootscreen_logo_raw[1] != 'M') {
        kfree(bootscreen_logo_raw); bootscreen_logo_raw = 0; return 0;
    }

    bootscreen_pixel_offset = *(uint32_t*)(bootscreen_logo_raw + 0x0A);
    bootscreen_logo_w = *(uint32_t*)(bootscreen_logo_raw + 0x12);
    bootscreen_logo_h = *(uint32_t*)(bootscreen_logo_raw + 0x16);
    uint16_t bpp = *(uint16_t*)(bootscreen_logo_raw + 0x1C);

    if (bpp != 24) {
        kfree(bootscreen_logo_raw); bootscreen_logo_raw = 0; return 0;
    }
    return 1;
}

void draw_bootscreen_background() {
    draw_rect(0, 0, fb_width, fb_height, COLOR_SKY_BLUE);

    if (!bootscreen_logo_raw) {
        swap_buffers();
        return;
    }

    int center_x = (fb_width - bootscreen_logo_w) / 2;
    int center_y = (fb_height - bootscreen_logo_h) / 2;
    int row_padded = (bootscreen_logo_w * 3 + 3) & (~3);

    for (uint32_t y = 0; y < bootscreen_logo_h; y++) {
        uint32_t bmp_y = (bootscreen_logo_h - 1) - y; 
        for (uint32_t x = 0; x < bootscreen_logo_w; x++) {
            uint32_t bmp_idx = bootscreen_pixel_offset + (bmp_y * row_padded) + (x * 3);
            uint8_t b = bootscreen_logo_raw[bmp_idx];
            uint8_t g = bootscreen_logo_raw[bmp_idx + 1];
            uint8_t r = bootscreen_logo_raw[bmp_idx + 2];
            uint32_t color = (r << 16) | (g << 8) | b;
            draw_pixel(center_x + x, center_y + y, color);
        }
    }
    swap_buffers(); // Manda l'immagine finita a schermo
}

void bootscreen_status(const char* msg) {
    int text_x = 50;
    int text_y = fb_height - 60; // In basso
    int area_w = fb_width - 100;

    
    draw_rect(text_x, text_y - 5, area_w, 30, COLOR_SKY_BLUE);
    
    draw_prop_string_bold(msg, text_x, text_y, 0xFF000000);
    swap_buffers(); 
    
    // Ora generiamo un'attesa per permettere all'utente di leggere cosa sta succedendo
    sleep(1000);
}

// ==========================================
// SOTTOSISTEMA PCI (Peripheral Component Interconnect)
// ==========================================

uint32_t pci_read_dword(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset) {
    uint32_t address = (uint32_t)((bus << 16) | (slot << 11) | (func << 8) | (offset & 0xFC) | ((uint32_t)0x80000000));
    outl(0xCF8, address);
    return inl(0xCFC);
}

uint16_t pci_read_word(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset) {
    uint32_t dword = pci_read_dword(bus, slot, func, offset);
    return (uint16_t)((dword >> ((offset & 2) * 8)) & 0xFFFF);
}

void pci_scan() {
    print_term("Scansione Bus PCI in corso...");
    int found = 0;

    for (uint16_t bus = 0; bus < 256; bus++) {
        for (uint8_t slot = 0; slot < 32; slot++) {
            uint16_t vendor = pci_read_word(bus, slot, 0, 0);
            
            if (vendor != 0xFFFF) {
                uint16_t device = pci_read_word(bus, slot, 0, 2);
                uint16_t class_subclass = pci_read_word(bus, slot, 0, 10);
                uint8_t class_id = (class_subclass >> 8) & 0xFF;

                char msg[100];
                char v_str[16], d_str[16];
                itoa_hex(vendor, v_str);
                itoa_hex(device, d_str);

                int idx = 0;
                char* l1 = "Trovato -> Vendor: "; while(*l1) msg[idx++] = *l1++;
                char* v = v_str; while(*v) msg[idx++] = *v++;
                char* l2 = " Dev: "; while(*l2) msg[idx++] = *l2++;
                char* d = d_str; while(*d) msg[idx++] = *d++;
                
                if (class_id == 0x04) {
                    char* l3 = " [ SCHEDA AUDIO! ]"; while(*l3) msg[idx++] = *l3++;
                }
                
                msg[idx] = '\0';
                print_term(msg);
                found++;
            }
        }
    }
    
    if (found == 0) print_term("Nessuna periferica PCI trovata.");
}

// ==========================================
// REAL TIME CLOCK (CMOS)
// ==========================================
uint8_t rtc_second, rtc_minute, rtc_hour, rtc_day, rtc_month, rtc_year;

// Legge un registro specifico dal chip CMOS
uint8_t get_rtc_register(int reg) {
    outb(0x70, reg);
    return inb(0x71);
}

// Controlla se l'orologio sta aggiornando i dati proprio in questo istante
int get_rtc_update_in_progress() {
    outb(0x70, 0x0A);
    return (inb(0x71) & 0x80);
}

// Funzione principale che legge data e ora perfette
void read_rtc() {
    // Aspettiamo che il chip abbia finito di "ticchettare" per non leggere dati a metà
    while (get_rtc_update_in_progress());

    rtc_second = get_rtc_register(0x00);
    rtc_minute = get_rtc_register(0x02);
    rtc_hour   = get_rtc_register(0x04);
    rtc_day    = get_rtc_register(0x07);
    rtc_month  = get_rtc_register(0x08);
    rtc_year   = get_rtc_register(0x09);

    uint8_t registerB = get_rtc_register(0x0B);

    // Se i dati sono nel vecchio formato BCD, li convertiamo nel nostro formato decimale standard
    if (!(registerB & 0x04)) {
        rtc_second = (rtc_second & 0x0F) + ((rtc_second / 16) * 10);
        rtc_minute = (rtc_minute & 0x0F) + ((rtc_minute / 16) * 10);
        rtc_hour = ( (rtc_hour & 0x0F) + (((rtc_hour & 0x70) / 16) * 10) ) | (rtc_hour & 0x80);
        rtc_day = (rtc_day & 0x0F) + ((rtc_day / 16) * 10);
        rtc_month = (rtc_month & 0x0F) + ((rtc_month / 16) * 10);
        rtc_year = (rtc_year & 0x0F) + ((rtc_year / 16) * 10);
    }
}



// ==========================================
// LETTURA RAM HARDWARE (CMOS)
// ==========================================
uint32_t detect_system_ram() {
    // Registri 0x34 e 0x35 del CMOS contengono la memoria oltre i 16MB (in blocchi da 64KB)
    outb(0x70, 0x34);
    uint8_t ext_lo = inb(0x71);
    outb(0x70, 0x35);
    uint8_t ext_hi = inb(0x71);
    
    // Uniamo le due metà (Bitwise shift)
    uint32_t ext_mem_64k = (ext_hi << 8) | ext_lo;
    
    if (ext_mem_64k > 0) {
        // RAM = 16 MB di base + i blocchi estesi convertiti in MB
        return 16 + ((ext_mem_64k * 64) / 1024);
    }
    
    // Fallback per PC storici con RAM < 16MB (Registri 0x30 e 0x31)
    outb(0x70, 0x30);
    uint8_t low = inb(0x71);
    outb(0x70, 0x31);
    uint8_t high = inb(0x71);
    uint32_t ext_mem_kb = (high << 8) | low;
    
    return 1 + (ext_mem_kb / 1024);
}

// Piccola funzione di utilità per trasformare un numero in due caratteri di testo (es: 9 -> "09")
void put_2digits(uint8_t val, char* buf, int offset) {
    buf[offset] = '0' + (val / 10);
    buf[offset + 1] = '0' + (val % 10);
}



// Prende una finestra e la spinge in cima all'array
void bring_to_front(int win_idx) {
    // La tendina appartiene alla finestra che aveva il fuoco: se il fuoco si
    // sposta, resterebbe aperta sopra un'applicazione che non la possiede.
    open_menu_win = -1;
    open_menu_idx = -1;
    int pos = -1;
    for(int i = 0; i < MAX_WINDOWS; i++) { if (window_order[i] == win_idx) pos = i; }
    if (pos <= 0) return; // È già in cima, o non esiste!
    
    // Facciamo scivolare tutte le altre finestre di 1 posto verso il basso
    for(int i = pos; i > 0; i--) { window_order[i] = window_order[i-1]; }
    window_order[0] = win_idx; // Mettiamo la nostra in cima!

    // ========================================================
    // PROTEZIONE Z-INDEX DEI MENU
    // Array esplicito: proteggiamo SOLO i due menu
    // ========================================================
    int menus_to_protect[2] = {WIN_TYPE_START_MENU, WIN_TYPE_APP_MENU};
    
    for (int m = 0; m < 2; m++) {
        int menu_type = menus_to_protect[m];
        int menu_idx = -1;
        
        for(int i = 0; i < MAX_WINDOWS; i++) {
            if(windows[i].active && windows[i].type == menu_type) menu_idx = i;
        }
        
        if (menu_idx != -1) {
            int menu_pos = -1;
            for(int i = 0; i < MAX_WINDOWS; i++) { if(window_order[i] == menu_idx) menu_pos = i; }
            
            if (menu_pos > 0) {
                for(int i = menu_pos; i > 0; i--) { window_order[i] = window_order[i-1]; }
                window_order[0] = menu_idx; // I Menu si riprendono la priorita'
            }
        }
    }
}


int global_creation_count = 0;

// Trova le finestre della Taskbar e le ordina cronologicamente
int get_taskbar_windows(int* valid_wins_array) {
    int count = 0;
    for(int i = 0; i < MAX_WINDOWS; i++) {
        if(windows[i].active && windows[i].type != WIN_TYPE_START_MENU && windows[i].type != WIN_TYPE_APP_MENU) {
            valid_wins_array[count++] = i;
        }
    }
    // Bubble Sort in base all'ID di creazione (dal più vecchio al più nuovo)
    for (int i = 0; i < count - 1; i++) {
        for (int j = 0; j < count - i - 1; j++) {
            if (windows[valid_wins_array[j]].creation_id > windows[valid_wins_array[j+1]].creation_id) {
                int temp = valid_wins_array[j];
                valid_wins_array[j] = valid_wins_array[j+1];
                valid_wins_array[j+1] = temp;
            }
        }
    }
    return count;
}


// Cerca uno slot libero e crea una nuova finestra
int open_window(int type, const char* title, int x, int y, int w, int h, int owner_pid) {
    
    // Iniziamo da 1. Lo slot 0 è permanentemente riservato 
    // per la logica dello "Zero" dell'SDK (Finestra Madre)
    for(int i = 1; i < MAX_WINDOWS; i++) {
        if (windows[i].active == 0) { 
            windows[i].active = 1;
            windows[i].type = type;

            // Gli slot delle finestre vengono riciclati: senza questo azzeramento
            // una nuova app eredita i menu di chi occupava lo slot prima di lei,
            // e non ha modo di cancellarli perche' un'app senza menu non chiama
            // mai la syscall che li dichiara.
            windows[i].menu_count = 0;
            windows[i].menu_action = 0;
            for (int m = 0; m < 6; m++) {
                windows[i].menu_item_count[m] = 0;
                windows[i].menu_titles[m][0] = '\0';
            }

            // Lo slot viene riciclato: la memoria della finestra precedente
            // non vale piu' e va restituita.
            if (windows[i].cache) { kfree(windows[i].cache); windows[i].cache = 0; }
            windows[i].cache_ok = 0;
            
            // ========================================================================
            // PUNTO 2: CENTRATURA AUTOMATICA ALL'APERTURA
            // Se non è un menu di sistema, posiziona la finestra al centro del desktop
            // ========================================================================
            if (type != WIN_TYPE_START_MENU && type != WIN_TYPE_APP_MENU) {
                x = (fb_width - w) / 2;
                y = (fb_height - 30 - h) / 2; // Centrato nell'area utile sopra la taskbar
                if (x < 0) x = 0;
                if (y < 0) y = 0;
            }
            
            windows[i].x = x; windows[i].y = y;
            windows[i].w = w; windows[i].h = h;
            windows[i].creation_id = global_creation_count++;
            windows[i].owner_pid = owner_pid; // SALVIAMO IL PROPRIETARIO

            // ========================================================================
            // PUNTO 1: Salviamo la dimensione di partenza come base personalizzata
            // ========================================================================
            windows[i].base_w = w;
            windows[i].base_h = h;
            windows[i].min_w = w; 
            windows[i].min_h = h;
            windows[i].dev_min_w = 0;   // lo slot potrebbe essere riciclato
            windows[i].dev_min_h = 0;


            // Aree di testo: il buffer della finestra precedente in questo
            // slot va restituito, e le aree ripartono vuote.
            for (int t = 0; t < TA_MAX; t++) {
                if (windows[i].ta[t].buf) kfree(windows[i].ta[t].buf);
                windows[i].ta[t].buf = 0;
                windows[i].ta[t].len = 0;
                windows[i].ta[t].cursor = 0;
                windows[i].ta[t].scroll = 0;
                windows[i].ta[t].version++;
            }
            
            // ASSEGNAZIONE INTELLIGENTE DELL'ICONA IN BASE AL TIPO
            if (type == WIN_TYPE_TERMINAL) windows[i].icon_id = 1; // Icona Terminale (1)
            else windows[i].icon_id = 0; // Icona Finestra Generica (0)

            
            // Copiamo il percorso completo per la nuvoletta (max 63 caratteri)
            int p = 0; while (title[p] != '\0' && p < 63) { windows[i].full_path[p] = title[p]; p++; }
            windows[i].full_path[p] = '\0';

            // Estraiamo solo il nome finale per la barra del titolo (es: "editor.edxi")
            int last_slash = -1;
            for (int k = 0; k < p; k++) { if (windows[i].full_path[k] == '/') last_slash = k; }
            
            int t = 0;
            if (last_slash != -1) {
                int start = last_slash + 1; // Partiamo da dopo lo slash
                while(windows[i].full_path[start] != '\0' && t < 31) { windows[i].title[t++] = windows[i].full_path[start++]; }
            } else {
                while(windows[i].full_path[t] != '\0' && t < 31) { windows[i].title[t] = windows[i].full_path[t]; t++; }
            }
            windows[i].title[t] = '\0';

            // Svuotiamo la memoria privata della finestra
            windows[i].t_len = 0;
            windows[i].t_cursor = 0;
            windows[i].t_scroll = 0;
            windows[i].t_head = 0; // Azzera l'indice
            windows[i].ui_count = 0;
            
            // FIX VISIBILITÀ MENU/TERMINALI:
            // Solo le App personalizzate (APP_GUI) partono nascoste in attesa del setup.
            // I menu di sistema e i terminali nascono immediatamente visibili!
            windows[i].is_initializing = (type == WIN_TYPE_APP_GUI) ? 1 : 0; 
            
            windows[i].is_maximized = 0; // Partono sempre in modalità normale
            windows[i].is_minimized = 0; 
            for(int r=0; r<TERM_MAX_ROWS; r++) windows[i].t_history[r][0] = '\0';

            bring_to_front(i); // Quando apro un'app, appare subito in primo piano
            global_active_win = i; // Prende subito il Focus
            return i; // Ritorna l'ID (l'indice) della nuova finestra
        }
    }
    return -1; // Errore: Troppe finestre aperte
}

// Inizializza il Desktop all'avvio
void init_window_manager() {
    // Svuotiamo la memoria
    for(int i = 0; i < MAX_WINDOWS; i++) {
        windows[i].active = 0;
        window_order[i] = i; // Inizialmente l'ordine è 0, 1, 2, 3...
    }
    
    // Qui c'era il comando per aprire un terminale all'avvio. Un passaggio storico
    // che rimane un ricordo di quando il sistema non aveva nemmeno un menu
    // (quindi l'ho commentato anziché eliminarlo)
    //open_window(WIN_TYPE_TERMINAL, "Ante-M OS x86", 262, 184, 500, 400);
    
}

// --- GESTORE DELLE SYSCALL 0x80 (Modello Linux Standard) ---



// Aggiorna magneticamente il target della Taskbar
void update_taskbar_state() {
    int needs_docking = 0;
    for(int i = 0; i < MAX_WINDOWS; i++) {
        if (windows[i].active && !windows[i].is_minimized && windows[i].is_maximized) {
            if (windows[i].type != WIN_TYPE_START_MENU && windows[i].type != WIN_TYPE_APP_MENU) {
                needs_docking = 1;
                break;
            }
        }
    }
    // Se c'è una finestra massimizzata va a 0 (tocca il fondo), altrimenti torna a 5 (fluttua)
    taskbar_target_offset = needs_docking ? 0 : 5;
}

void draw_top_panel() {
    uint32_t title_light = active_theme.titlebar_base;
    uint32_t title_dark  = active_theme.titlebar_lines;

    // A. Sfondo barra a tutta larghezza
    draw_rect(0, 0, fb_width, PANEL_BAR_H, title_light);

    // B. Linee decorative scure (stesso passo 6px della barra del titolo)
    for (int ly = 1; ly < PANEL_BAR_H; ly += 6) {
        int h = 4;
        if (ly + h > PANEL_BAR_H) h = PANEL_BAR_H - ly;
        if (h > 0) draw_rect(0, ly, fb_width, h, title_dark);
    }

    // C. Gradiente di luce (bianco che sfuma verso il basso)
    for (int py = 0; py < PANEL_BAR_H; py++) {
        int white_mix = 30 - (py * 30 / PANEL_BAR_H);
        if (white_mix <= 0) continue;
        uint32_t* row = &backbuffer[py * fb_width];
        for (int px = 0; px < fb_width; px++) {
            uint32_t cp = row[px];
            uint32_t r = (cp >> 16) & 0xFF, g = (cp >> 8) & 0xFF, b = cp & 0xFF;
            r += (255 - r) * white_mix / 100;
            g += (255 - g) * white_mix / 100;
            b += (255 - b) * white_mix / 100;
            row[px] = 0xFF000000 | (r << 16) | (g << 8) | b;
        }
    }

    // D. Ombra nera sfumata sotto la barra
    for (int i = 0; i < PANEL_SHADOW_H; i++) {
        int py = PANEL_BAR_H + i;
        if (py < 0 || py >= fb_height) continue;
        int f = (PANEL_SHADOW_H - i) * 100 / PANEL_SHADOW_H;
        int dark = 28 * f / 100 * f / 100;      // % di nero, caduta quadratica
        uint32_t* row = &backbuffer[py * fb_width];
        for (int px = 0; px < fb_width; px++) {
            uint32_t cp = row[px];
            uint32_t r = ((cp >> 16) & 0xFF) * (100 - dark) / 100;
            uint32_t g = ((cp >> 8) & 0xFF) * (100 - dark) / 100;
            uint32_t b = (cp & 0xFF) * (100 - dark) / 100;
            row[px] = 0xFF000000 | (r << 16) | (g << 8) | b;
        }
    }
}

// Radice quadrata intera (Newton) per il raccordo curvo dell'ombra (serve per l'omnbra delle finestre)
static int shadow_isqrt(int n) {
    if (n <= 0) return 0;
    int x = n, y = (x + 1) / 2;
    while (y < x) { x = y; y = (x + n / x) / 2; }
    return x;
}

// ----------------------------------------------------------------------------
// Ombra delle finestre, precalcolata. L'intensita' dipende solo da quanto un
// pixel dista dal bordo della finestra (dx di lato, dy sotto), non dalle sue
// dimensioni: una sola tabella 13x13 vale per tutte le finestre. Prima si
// calcolava per ogni pixel una radice quadrata col metodo di Newton, cioe' una
// catena di divisioni, che sotto emulazione sono tra le operazioni piu' lente.
// Il risultato e' identico al pixel.
// ----------------------------------------------------------------------------
#define SH_SIDE    8     // estensione ombre laterali
#define SH_BOT     12    // estensione ombra inferiore
#define SH_D       26    // intensita' massima (% di nero)
#define SH_TOP_OFF 6     // le ombre laterali partono qualche px piu' in basso
#define SH_REACH   12    // il maggiore fra SH_SIDE e SH_BOT

static uint8_t sh_tab[SH_REACH + 1][SH_REACH + 1];   // [dy][dx] -> % di nero
static int sh_ready = 0;

static void shadow_build(void) {
    for (int dy = 0; dy <= SH_REACH; dy++) {
        for (int dx = 0; dx <= SH_REACH; dx++) {
            int nx = dx * 100 / SH_SIDE;
            int ny = dy * 100 / SH_BOT;
            int nd = shadow_isqrt(nx * nx + ny * ny);
            int dark = 0;
            if ((dx || dy) && nd < 100) {
                int f = 100 - nd;                    // 100 al bordo, 0 al limite
                dark = SH_D * f / 100 * f / 100;     // caduta quadratica
            }
            sh_tab[dy][dx] = (uint8_t)dark;
        }
    }
    sh_ready = 1;
}

void draw_window_shadow(int x, int y, int w, int h) {
    if (!sh_ready) shadow_build();

    int x0 = x - SH_REACH, x1 = x + w + SH_REACH;
    int y0 = y, y1 = y + h + SH_REACH;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > fb_width)  x1 = fb_width;
    if (y1 > fb_height) y1 = fb_height;

    for (int py = y0; py < y1; py++) {
        uint32_t* row = &backbuffer[py * fb_width];
        int dy = (py >= y + h) ? py - (y + h) + 1 : 0;

        // Accanto alla finestra le ombre laterali hanno un imbocco sfumato che
        // dipende solo dalla riga: si calcola una volta per riga, non per pixel.
        int fade = SH_SIDE;                       // SH_SIDE = nessuna attenuazione
        if (dy == 0) {
            int r = py - (y + SH_TOP_OFF);
            if (r < 0) continue;                  // primissime righe: nessuna ombra
            if (r < SH_SIDE) fade = r;
        }

        for (int px = x0; px < x1; px++) {
            // L'interno della finestra non va nemmeno visitato
            if (dy == 0 && px >= x && px < x + w) { px = x + w - 1; continue; }

            int dx = 0;
            if (px < x)           dx = x - px;
            else if (px >= x + w) dx = px - (x + w) + 1;

            int dark = sh_tab[dy][dx];
            if (fade < SH_SIDE) dark = dark * fade / SH_SIDE;
            if (dark <= 0) continue;

            uint32_t cp = row[px];
            uint32_t r = ((cp >> 16) & 0xFF) * (100 - dark) / 100;
            uint32_t g = ((cp >> 8)  & 0xFF) * (100 - dark) / 100;
            uint32_t b = (cp & 0xFF)         * (100 - dark) / 100;
            row[px] = 0xFF000000 | (r << 16) | (g << 8) | b;
        }
    }
}

// ============================================================================
// AREA DI TESTO MULTILINEA: impaginazione, cursore, tastiera, click
// ============================================================================
#define TA_LINE_H 16      // altezza di una riga
#define TA_SB_W   8       // larghezza della barra di scorrimento

// Inizi di riga dopo l'andata a capo. Un solo array condiviso: il kernel ha un
// solo ciclo principale, quindi due impaginazioni non avvengono mai insieme.
static int ta_starts[TA_CAP + 2];

// ============================================================================
// TESTO: DA UTF-8 A LATIN-1
// Dentro il sistema ogni carattere occupa un solo byte, con la codifica
// Latin-1 (ISO-8859-1): le lettere accentate stanno fra 192 e 255, ed e' li'
// che il font le disegna. Il testo che arriva da fuori - le stringhe delle app,
// che il compilatore scrive in UTF-8, e le risposte di Ollama - si converte
// qui, una volta sola, all'ingresso. Cosi' larghezze, andate a capo e cursori
// continuano a contare un byte per carattere.
// ============================================================================

// Un carattere Unicode nel byte Latin-1 che gli somiglia di piu'.
// 0 = da tralasciare (le emoji); '?' = non rappresentabile.
static unsigned char latin1_from_cp(uint32_t cp) {
    if (cp < 0x80) return (unsigned char)cp;
    if (cp == 0xA0) return ' ';                          // spazio non separabile
    if (cp > 0xA0 && cp <= 0xFF) return (unsigned char)cp;
    switch (cp) {
        // La punteggiatura "tipografica" che i modelli linguistici amano tanto
        case 0x2018: case 0x2019: case 0x201A: case 0x2032: return '\'';
        case 0x201C: case 0x201D: case 0x201E: case 0x2033: return '"';
        case 0x2013: case 0x2014: case 0x2212:              return '-';
        case 0x2022:                                        return '*';
        case 0x2026:                                        return '.';
    }
    if (cp >= 0x10000) return 0;
    return '?';
}

// Legge un carattere UTF-8 da s (n byte disponibili al massimo) e dice quanti
// ne ha consumati. Un byte che non apre una sequenza UTF-8 valida si prende
// cosi' com'e': e' gia' Latin-1, e passare due volte di qui non lo rovina.
static unsigned char utf8_take(const unsigned char* s, int n, int* used) {
    unsigned char c = s[0];
    if (c < 0x80) { *used = 1; return c; }
    if ((c & 0xE0) == 0xC0 && n >= 2 && (s[1] & 0xC0) == 0x80) {
        *used = 2;
        return latin1_from_cp(((uint32_t)(c & 0x1F) << 6) | (s[1] & 0x3F));
    }
    if ((c & 0xF0) == 0xE0 && n >= 3 && (s[1] & 0xC0) == 0x80 && (s[2] & 0xC0) == 0x80) {
        *used = 3;
        return latin1_from_cp(((uint32_t)(c & 0x0F) << 12) |
                              ((uint32_t)(s[1] & 0x3F) << 6) | (s[2] & 0x3F));
    }
    if ((c & 0xF8) == 0xF0 && n >= 4 && (s[1] & 0xC0) == 0x80 &&
        (s[2] & 0xC0) == 0x80 && (s[3] & 0xC0) == 0x80) {
        *used = 4;
        return 0;                                        // emoji: si tralascia
    }
    *used = 1;
    return c;                                            // gia' Latin-1
}

// Copia src in dst convertendolo. src_max limita la lettura, per il testo che
// arriva dalla memoria di un'app e potrebbe non essere terminato.
static int utf8_to_latin1(char* dst, int dst_max, const char* src, int src_max) {
    const unsigned char* s = (const unsigned char*)src;
    int i = 0, o = 0;
    while (i < src_max && s[i] && o < dst_max - 1) {
        int used;
        unsigned char c = utf8_take(s + i, src_max - i, &used);
        i += used;
        if (c) dst[o++] = (char)c;
    }
    dst[o] = '\0';
    return o;
}

// Aggiunge testo UTF-8 in coda a un'area di testo, convertendolo
static void ta_append_text(textarea_t* ta, const char* src) {
    const unsigned char* s = (const unsigned char*)src;
    int i = 0;
    while (s[i] && ta->len < TA_CAP) {
        int used;
        unsigned char c = utf8_take(s + i, 4, &used);    // stringa terminata: bastano 4
        i += used;
        if (c) ta->buf[ta->len++] = (char)c;
    }
}

static int ta_char_w(unsigned char c) {
    if (c == ' ') return 5;
    if (FONT_HAS(c)) return font_prop[c].width + 1;
    return 8;
}

static int ta_inner_w(gui_element_t* el) { return el->w - 8 - TA_SB_W; }

static int ta_visible(gui_element_t* el) {
    int v = (el->h - 8) / TA_LINE_H;
    return v < 1 ? 1 : v;
}

static void ta_alloc(textarea_t* ta) {
    ta->buf = (char*)kmalloc(TA_CAP + 1);
    if (ta->buf) ta->buf[0] = '\0';
    ta->len = 0;
    ta->cursor = 0;
    ta->scroll = 0;
    ta->version++;
}

// Divide il testo in righe che stanno nella larghezza data. Una riga finisce a
// un '\n' oppure quando la parola successiva non ci sta piu': in quel caso si
// va a capo dopo l'ultimo spazio, e una parola piu' lunga dell'intera riga
// viene spezzata di netto. Restituisce il numero di righe.
static int ta_layout(textarea_t* ta, int width) {
    const char* s = ta->buf;
    int len = ta->len;
    int n = 0;
    int ls = 0, x = 0, last_sp = -1, i = 0;

    ta_starts[n++] = 0;
    while (i < len && n < TA_CAP + 1) {
        unsigned char c = (unsigned char)s[i];
        if (c == '\n') {
            ls = i + 1;
            ta_starts[n++] = ls;
            x = 0;
            last_sp = -1;
            i++;
            continue;
        }
        int cw = ta_char_w(c);
        if (x + cw > width && i > ls) {
            ls = (last_sp >= ls) ? last_sp + 1 : i;
            ta_starts[n++] = ls;
            // La nuova riga comincia con il pezzo di parola gia' percorso
            x = 0;
            last_sp = -1;
            for (int k = ls; k < i; k++) {
                x += ta_char_w((unsigned char)s[k]);
                if (s[k] == ' ') last_sp = k;
            }
            continue;   // si rivaluta lo stesso carattere sulla riga nuova
        }
        if (c == ' ') last_sp = i;
        x += cw;
        i++;
    }
    return n;
}

// Riga che contiene la posizione pos
static int ta_line_of(int n, int pos) {
    int L = 0;
    while (L + 1 < n && ta_starts[L + 1] <= pos) L++;
    return L;
}

// Fine della riga L, escluso l'eventuale '\n' che la chiude
static int ta_line_end(textarea_t* ta, int n, int L) {
    int e = (L + 1 < n) ? ta_starts[L + 1] : ta->len;
    if (e > ta_starts[L] && ta->buf[e - 1] == '\n') e--;
    return e;
}

// Distanza in pixel fra l'inizio della riga L e la posizione pos
static int ta_x_of(textarea_t* ta, int L, int pos) {
    int x = 0;
    for (int i = ta_starts[L]; i < pos && i < ta->len; i++)
        x += ta_char_w((unsigned char)ta->buf[i]);
    return x;
}

// Posizione nel testo piu' vicina alla coordinata px sulla riga L
static int ta_pos_at_x(textarea_t* ta, int n, int L, int px) {
    int b = ta_line_end(ta, n, L);
    int x = 0;
    for (int i = ta_starts[L]; i < b; i++) {
        int cw = ta_char_w((unsigned char)ta->buf[i]);
        if (px < x + cw / 2) return i;
        x += cw;
    }
    return b;
}

static int ta_readonly(gui_element_t* el) { return (el->color2 & 0x100) != 0; }

// Se il fuoco e' su un'area di testo della finestra in primo piano, la restituisce
static int ta_focus(int top_win, textarea_t** ta, gui_element_t** el) {
    if (focused_window_id < 0 || focused_window_id != top_win || focused_element_idx < 0) return 0;
    window_t* fw = &windows[focused_window_id];
    if (focused_element_idx >= fw->ui_count) return 0;
    gui_element_t* e = &fw->ui_elements[focused_element_idx];
    if (e->type != 6) return 0;
    int t = e->color2 & 0xFF;
    if (t >= TA_MAX || !fw->ta[t].buf) return 0;
    *ta = &fw->ta[t];
    *el = e;
    return 1;
}

// Frecce sinistra/destra: il cursore scorre nel testo
static void ta_move_h(textarea_t* ta, gui_element_t* el, int d) {
    if (ta_readonly(el)) return;
    ta->cursor += d;
    if (ta->cursor < 0) ta->cursor = 0;
    if (ta->cursor > ta->len) ta->cursor = ta->len;
    ta->version++;
}

// Frecce su/giu' e pagine. In sola lettura non c'e' un cursore da spostare:
// si fa scorrere il testo. Altrimenti il cursore cambia riga cercando di
// restare alla stessa distanza dal margine sinistro.
static void ta_move_v(textarea_t* ta, gui_element_t* el, int d) {
    if (ta_readonly(el)) {
        ta->scroll += d;
        if (ta->scroll < 0) ta->scroll = 0;
        ta->version++;
        return;
    }
    int n = ta_layout(ta, ta_inner_w(el));
    int L = ta_line_of(n, ta->cursor);
    int x = ta_x_of(ta, L, ta->cursor);
    int T = L + d;
    if (T < 0) T = 0;
    if (T >= n) T = n - 1;
    ta->cursor = ta_pos_at_x(ta, n, T, x);
    ta->version++;
}

// Un carattere digitato: inserimento, cancellazione, ritorno a capo
static void ta_key(textarea_t* ta, gui_element_t* el, char c) {
    if (ta_readonly(el)) return;
    if (c == '\b') {
        if (ta->cursor > 0) {
            for (int k = ta->cursor; k <= ta->len; k++) ta->buf[k - 1] = ta->buf[k];
            ta->cursor--;
            ta->len--;
        }
    } else if (c == '\n' || (c >= 32 && c <= 126)) {
        if (ta->len < TA_CAP) {
            for (int k = ta->len; k >= ta->cursor; k--) ta->buf[k + 1] = ta->buf[k];
            ta->buf[ta->cursor++] = c;
            ta->len++;
        }
    }
    ta->version++;
}

// Click dentro l'area: sulla barra di scorrimento sfoglia di una pagina, sul
// testo posiziona il cursore. rx e ry sono relativi all'angolo dell'area.
static void ta_click(textarea_t* ta, gui_element_t* el, int rx, int ry) {
    int vis = ta_visible(el);
    if (rx >= el->w - 2 - TA_SB_W) {
        ta->scroll += (ry < el->h / 2) ? -vis : vis;
        if (ta->scroll < 0) ta->scroll = 0;
    } else if (!ta_readonly(el)) {
        int n = ta_layout(ta, ta_inner_w(el));
        int L = ta->scroll + (ry - 4) / TA_LINE_H;
        if (L < 0) L = 0;
        if (L >= n) L = n - 1;
        ta->cursor = ta_pos_at_x(ta, n, L, rx - 4);
    }
    ta->version++;
}


// ----------------------------------------------------------------------------
// Finestra di applicazione (WIN_TYPE_APP_GUI): disegna gli elementi inviati
// dall'app via syscall (rettangoli, bottoni, testo, textbox) e la scrollbar.
// 'w' e' l'indice della finestra: serve per i confronti di focus/proprieta'.
// ----------------------------------------------------------------------------
static void render_app_gui(window_t* win, int w, int top_app_win,
                           int mouse_x, int mouse_y, int is_clicking) {

            
    // 1. Coordinate Logiche (Intatte per non sfasare i bottoni delle App)
    int app_x = win->x + 4;
    int app_y = win->y + 32; 
    
    // 2. Coordinate Fisiche della Tela (Incastro perfetto a 1px)
    int canvas_x = win->x + 1;
    int canvas_y = win->y + 32;
    int canvas_w = win->w - 2;
    int canvas_h = win->h - 33; 
    
    uint32_t bg_color = 0xFFFFFFFF; // Bianco Puro

    // ========================================================================
    // A. SFONDO CENTRALE PINSTRIPE (Righe da 2 pixel)
    // ========================================================================
    draw_rect(canvas_x, canvas_y, canvas_w, canvas_h, bg_color);

    uint32_t pinstripe_color = 0xFFE8E8E8; 
    
    // PASSO DI 4 PIXEL: Disegna 2 pixel di riga grigia e lascia 2 pixel di spazio bianco!
    for (int ly = canvas_y + 2; ly < canvas_y + canvas_h; ly += 4) {
        int h = 2; // Spessore della linea aumentato a 2 pixel
        
        // Sicurezza: se la linea sborda fuori dal basso della finestra, la tagliamo
        if (ly + h > canvas_y + canvas_h) h = (canvas_y + canvas_h) - ly;
        
        draw_rect(canvas_x, ly, canvas_w, h, pinstripe_color);
    }

    // IL BLOCCO "B" DEI VECCHI ANGOLI INFERIORI È STATO ELIMINATO!
    // La finestra è squadrata e pulita.
    
    
    
    
    
    // Disegna tutto quello che l'App ha ordinato nella sua Display List
    for(int e = 0; e < win->ui_count; e++) {
        gui_element_t* el = &win->ui_elements[e];
        int ex = app_x + el->x;
        int ey = app_y + el->y;
        
        if (el->type == 1) { // 1 = Pannello Satinato Dinamico
            uint32_t p_br = (el->color1 >> 16) & 0xFF;
            uint32_t p_bg = (el->color1 >> 8) & 0xFF;
            uint32_t p_bb = el->color1 & 0xFF;

            // Clipping di sicurezza per non disegnare fuori dal monitor
            int p_sx = ex; if (p_sx < 0) p_sx = 0;
            int p_sy = ey; if (p_sy < 0) p_sy = 0;
            int p_ex = ex + el->w; if (p_ex > fb_width) p_ex = fb_width;
            int p_ey = ey + el->h; if (p_ey > fb_height) p_ey = fb_height;

            for (int py = p_sy; py < p_ey; py++) {
                uint32_t* row_ptr = &backbuffer[py * fb_width];
                for (int px = p_sx; px < p_ex; px++) {
                    // Mappiamo la texture usando le coordinate relative al pannello
                    uint32_t rel_x = px - ex; 
                    uint32_t rel_y = py - ey;
                    uint32_t n = rel_x + (rel_y * 57325);
                    n = (n << 13) ^ n;
                    uint32_t nn = (n * (n * n * 15731 + 789221) + 1376312589);
                    
                    // Escursione di +/- 18 sul colore base
                    int shift = ((nn % 7) - 3) * 6; 
                    
                    int mod_r = p_br + shift;
                    int mod_g = p_bg + shift;
                    int mod_b = p_bb + shift;
                    
                    // Clipping anti-corruzione per colori estremi (bianco o nero)
                    if (mod_r < 0) mod_r = 0; else if (mod_r > 255) mod_r = 255;
                    if (mod_g < 0) mod_g = 0; else if (mod_g > 255) mod_g = 255;
                    if (mod_b < 0) mod_b = 0; else if (mod_b > 255) mod_b = 255;

                    row_ptr[px] = 0xFF000000 | (mod_r << 16) | (mod_g << 8) | mod_b;
                }
            }
        }
        else if (el->type == 2) { // 2 = Bottone 3D
            // Rileviamo se il mouse sta fisicamente premendo QUESTO bottone
            int is_pressed = 0;
            if (is_clicking && w == top_app_win && mouse_x >= ex && mouse_x <= ex + el->w && mouse_y >= ey && mouse_y <= ey + el->h) {
                is_pressed = 1;
            }
            
            uint32_t light  = 0xFFFFFFFF;
            uint32_t shadow = 0xFF575757;

            // BUTTON_DEFAULT (0) = colore del tema. Qualunque altro valore e'
            // una scelta dell'app: velo, bordo e stato premuto si ricavano da li'.
            uint32_t body, veil, rim, body_pressed;
            if (el->color1 == BUTTON_DEFAULT) {
                body         = active_theme.button_body;
                body_pressed = active_theme.button_body_pressed;
                veil         = active_theme.button_veil;
                rim          = active_theme.button_rim;
            } else {
                body         = el->color1;
                body_pressed = gfx_derive_pressed(body);
                veil         = gfx_derive_veil(body);
                rim          = gfx_derive_rim(body);
            }

            uint32_t btn_bg = body;
            if (is_pressed) {
                // draw_button() rialza il corpo di 4/3 quando il light non e'
                // bianco: pre-scuriamo di 3/4 per ottenere il colore voluto.
                btn_bg = 0xFF000000
                    | ((((body_pressed >> 16) & 0xFF) * 3 / 4) << 16)
                    | ((((body_pressed >>  8) & 0xFF) * 3 / 4) <<  8)
                    |  ((  body_pressed        & 0xFF) * 3 / 4);
                light = 0xFFC0C0C0;
            }

            gfx_style_aqua(veil, rim);
            draw_button(ex, ey, el->w, el->h, btn_bg, light, shadow);
            draw_button_rays(ex, ey, el->w, el->h, RAYS_AUTO,
                             is_pressed ? 55 : 70, active_theme.button_rays);
            gfx_style_reset();
            
            // =======================================================
            // TESTO DEL BOTTONE (Fisso, incollato sotto al vetro!)
            // =======================================================
            int text_width_px = get_prop_string_width_bold(el->text);
            int text_x = ex + (el->w / 2) - (text_width_px / 2);
            int text_y = ey + (el->h / 2) - 10;   // alzato di 2px

            draw_prop_string_bold_shadow(el->text, text_x, text_y, 3, 0xFF000000, 24);
            draw_prop_string_bold(el->text, text_x, text_y, el->color2);
        }

        else if ((el->type & 0xFF) == 3) { // 3 = Testo Libero
            // Se il bit 0x0400 è acceso, usa la funzione Bold
            if (el->type & 0x0400) {
                draw_prop_string_bold(el->text, ex, ey, el->color1);
            } else {
                draw_prop_string(el->text, ex, ey, el->color1);
            }
        }
        
        else if (el->type == 4) {
            draw_textfield(ex, ey, el->w, el->h, el->color1); 
            
            // 1. Calcoliamo la larghezza del testo fino al cursore
            int cursor_offset = 0;
            for (int k = 0; k < focused_cursor_pos && el->text[k] != '\0'; k++) {
                unsigned char uc = (unsigned char)el->text[k];
                if (FONT_HAS(uc)) {
                    if (uc == ' ') cursor_offset += 5;
                    else cursor_offset += font_prop[uc].width + 1;
                } else cursor_offset += 8;
            }
            
            // 2. Calcolo Scorrimento Orizzontale (Scroll_X)
            int text_area_w = el->w - 8; 
            int scroll_x = 0;
            
            // Solo se questa textbox ha il focus ed è in uso, la facciamo scorrere
            if (w == top_app_win && focused_window_id == w && focused_element_idx == e) {
                if (cursor_offset > text_area_w - 12) {
                    scroll_x = cursor_offset - (text_area_w - 12); // Spingiamo il testo a sinistra
                }
            }

            // 3. DISEGNO TESTO TAGLIATO SUI BORDI (Clipping)
            draw_prop_string_clipped(el->text, ex + 4 - scroll_x, ey + 4, 0xFF000000, ex + 2, ey + 2, el->w - 4, el->h - 4);
            
            // =======================================================
            // 4. DISEGNO DEL CURSORE A BLOCCO (Proporzionale NATIVO)
            // =======================================================
            if (w == top_app_win && focused_window_id == w && focused_element_idx == e && blink_counter < 50) {
                
                int cx = ex + 4 + cursor_offset - scroll_x;
                int cy = ey + 4;
                char char_under_cursor = el->text[focused_cursor_pos];
                
                int block_w = 8; 
                if (char_under_cursor != '\0' && FONT_HAS((unsigned char)char_under_cursor)) {
                    if (char_under_cursor == ' ') block_w = 5;
                    else block_w = font_prop[(unsigned char)char_under_cursor].width + 1;
                }
                
                // Disegniamo il blocco SOLO se non sborda fuori dai margini della scatola
                if (cx >= ex + 2 && cx + block_w <= ex + el->w - 2) {
                    draw_rect(cx, cy, block_w, 16, 0xFF000000);
                    
                    // Invertiamo il colore stampandoci sopra (sempre col clipping!)
                    if (char_under_cursor != '\0') {
                        char temp_str[2] = {char_under_cursor, '\0'};
                        draw_prop_string_clipped(temp_str, cx, cy, 0xFFFFFFFF, ex + 2, ey + 2, el->w - 4, el->h - 4);
                    }
                }
            }
        }

        else if (el->type == 6) { // 6 = AREA DI TESTO MULTILINEA
            draw_textfield(ex, ey, el->w, el->h, el->color1);

            int ti = el->color2 & 0xFF;
            if (ti < TA_MAX && win->ta[ti].buf) {
                textarea_t* ta = &win->ta[ti];
                int ro  = ta_readonly(el);
                int n   = ta_layout(ta, ta_inner_w(el));
                int vis = ta_visible(el);
                int focused = (w == top_app_win && focused_window_id == w && focused_element_idx == e);
                int cl  = ta_line_of(n, ta->cursor);

                // Mentre si scrive, il cursore resta sempre in vista
                if (focused && !ro) {
                    if (cl < ta->scroll) ta->scroll = cl;
                    if (cl >= ta->scroll + vis) ta->scroll = cl - vis + 1;
                }
                int max_scroll = n - vis;
                if (max_scroll < 0) max_scroll = 0;
                if (ta->scroll > max_scroll) ta->scroll = max_scroll;
                if (ta->scroll < 0) ta->scroll = 0;

                // Righe visibili, tagliate prima della barra di scorrimento
                int clip_w = el->w - 4 - TA_SB_W;
                for (int L = ta->scroll; L < n && L < ta->scroll + vis; L++) {
                    int a = ta_starts[L], b = ta_line_end(ta, n, L);
                    char line[256];
                    int k = 0;
                    for (int i = a; i < b && k < 255; i++) line[k++] = ta->buf[i];
                    line[k] = '\0';
                    draw_prop_string_clipped(line, ex + 4, ey + 4 + (L - ta->scroll) * TA_LINE_H,
                                             0xFF000000, ex + 2, ey + 2, clip_w, el->h - 4);
                }

                // Cursore: una barretta sottile, piu' naturale del blocco quando
                // il testo va a capo e il cursore si muove anche in verticale
                if (focused && !ro && blink_counter < 50 &&
                    cl >= ta->scroll && cl < ta->scroll + vis) {
                    int cx = ex + 4 + ta_x_of(ta, cl, ta->cursor);
                    int cy = ey + 4 + (cl - ta->scroll) * TA_LINE_H;
                    if (cx < ex + 2 + clip_w) draw_rect(cx, cy, 2, TA_LINE_H, 0xFF000000);
                }

                // Barra di scorrimento, solo se il testo non ci sta tutto
                if (n > vis) {
                    int sx = ex + el->w - 2 - TA_SB_W;
                    int sy = ey + 2, sh = el->h - 4;
                    draw_rect(sx, sy, TA_SB_W, sh, 0xFFE0E0E0);
                    int th = sh * vis / n;
                    if (th < 12) th = 12;
                    int tyy = sy + (sh - th) * ta->scroll / max_scroll;
                    draw_rect(sx + 1, tyy, TA_SB_W - 2, th, 0xFF909090);
                }
            }
        }
        else if (el->type == 7) { // 7 = DISPLAY DI VETRO
            draw_glass_display(ex, ey, el->w, el->h, el->color1);

            // Cifre ingrandite se il display e' abbastanza alto, allineate a
            // destra come su ogni calcolatrice, centrate in verticale
            int sc = (el->h >= 48) ? 2 : 1;
            int tw = get_prop_string_width_bold_scaled(el->text, sc);
            // Un testo troppo lungo per le cifre grandi torna alla misura
            // normale, invece di uscire dal vetro
            if (sc == 2 && tw > el->w - 20) {
                sc = 1;
                tw = get_prop_string_width_bold_scaled(el->text, sc);
            }
            int tx = ex + el->w - 12 - tw;
            if (tx < ex + 8) tx = ex + 8;
            int ty = ey + (el->h - 16 * sc) / 2;

            draw_prop_string_bold_scaled(el->text, tx + 1, ty + 1, 0xFF000000, sc, 90);  // ombra
            draw_prop_string_bold_scaled(el->text, tx, ty, el->color2, sc, 255);
        }
        else if (el->type == 5) { // 5 = IMMAGINE RAW a 32-bit
            // 1. Cerchiamo chi è il proprietario di questa finestra
            int app_pid = -1;
            for(int p=1; p<MAX_PROCESSES; p++) {
                if (process_table[p].active && process_table[p].linked_window == w) app_pid = p;
            }

            if (app_pid > 0) {
                // ==========================================
                // INIZIO SEZIONE CRITICA (Protezione Timer)
                // Salviamo lo stato attuale del processore e disattiviamo gli interrupt!
                // ==========================================
                uint32_t flags;
                asm volatile("pushf \n pop %0 \n cli" : "=r"(flags));

                // 2. MAGIA: Colleghiamo il cervello del Kernel alla RAM privata di questa specifica App
                page_directory[256] = ((uint32_t)process_table[app_pid].page_table) | 0x07;
                asm volatile("mov %%cr3, %%eax; mov %%eax, %%cr3" ::: "eax"); // Flush TLB

                // 3. Ora possiamo leggere l'immagine in totale sicurezza
                uint32_t* pixels = (uint32_t*)el->color1;
                for (int iy = 0; iy < el->h; iy++) {
                    for (int ix = 0; ix < el->w; ix++) {
                        draw_pixel(ex + ix, ey + iy, pixels[iy * el->w + ix]);
                    }
                }

                // 4. Ripristiniamo la RAM dell'App attualmente in esecuzione nello scheduler
                if (current_pid > 0) page_directory[256] = ((uint32_t)process_table[current_pid].page_table) | 0x07;
                else page_directory[256] = 0;
                asm volatile("mov %%cr3, %%eax; mov %%eax, %%cr3" ::: "eax"); // Flush TLB

                // ==========================================
                // FINE SEZIONE CRITICA
                // Ripristiniamo gli interrupt esattamente come li avevamo trovati!
                // ==========================================
                asm volatile("push %0 \n popf" :: "r"(flags));
            }
        }
    }
}






// ----------------------------------------------------------------------------
// Menu Start (WIN_TYPE_START_MENU): fascia laterale in vetro Aqua verticale,
// banner "III MILLENNIUM" e voci del menu con hover zigrinato.
// ----------------------------------------------------------------------------
// ============================================================================
// MENU PRINCIPALE (si apre dal pulsante Main)
// Un solo pannello di vetro: in testa il nome del sistema su una lastra del
// display di vetro; a sinistra le applicazioni, con una ricerca che filtra
// mentre si scrive; a destra le voci di sistema; in fondo il pulsante Exit.
// Prende il posto del vecchio menu e del suo sottomenu Applications.
//
// Il menu non rientra nella memoria per finestra, quindi si arrangia da solo:
// vetro, ombra e intestazione non cambiano finche' resta aperto, e si
// disegnano una volta sola all'apertura per poi essere ricopiati. A ogni
// fotogramma si ridisegna soltanto cio' che si muove: ricerca, elenco,
// evidenziazione sotto il mouse e pulsante Exit.
// ============================================================================
#define SM_W          372
#define SM_H          292
#define SM_LIST_X     10                  // riquadro delle app, relativo al menu
#define SM_LIST_Y     60
#define SM_LIST_W     206
#define SM_LIST_H     (SM_H - 60 - 54)
#define SM_ROW_Y      (SM_LIST_Y + 34)    // prima riga dell'elenco
#define SM_ROW_H      26
#define SM_ROWS       5                   // righe visibili
#define SM_SYS_X      226                 // colonna delle voci di sistema
#define SM_EXIT_X     (SM_W - 110)
#define SM_EXIT_Y     (SM_H - 38)
#define SM_EXIT_W     98
#define SM_EXIT_H     28
#define SM_QUERY_MAX  20
#define SM_SHADOW     8                   // estensione dell'ombra

static uint32_t sm_exit_armed_until = 0;  // Exit chiede conferma per tre secondi
static int sm_hits[TERM_MAX_ROWS];        // app che corrispondono alla ricerca

// Voce premuta, in attesa del rilascio. Un valore >= 0 e' un'app (il suo
// indice nell'elenco completo); i negativi sono le altre voci.
#define SM_PRESS_NONE      (-1)
#define SM_PRESS_TERMINAL  (-2)
#define SM_PRESS_EXIT      (-3)
static int sm_press = SM_PRESS_NONE;

// Strato fisso del menu, ombra compresa, memorizzato all'apertura
static uint32_t* sm_bg = 0;
static int sm_bg_cap = 0;
static int sm_bg_ok = 0;
static int sm_bg_x, sm_bg_y, sm_bg_w, sm_bg_h;
static uint32_t sm_bg_theme = 0;

// ---- ricerca ---------------------------------------------------------------
static char sm_lower(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c; }

// La ricerca trova il testo in qualunque punto del nome, senza badare alle
// maiuscole: "lay" trova "player"
static int sm_match(const char* name, const char* q) {
    if (!q[0]) return 1;
    for (int i = 0; name[i]; i++) {
        int k = 0;
        while (q[k] && name[i + k] && sm_lower(name[i + k]) == sm_lower(q[k])) k++;
        if (!q[k]) return 1;
    }
    return 0;
}

static int sm_filter(window_t* win) {
    int n = 0;
    for (int i = 0; i < win->t_len && n < TERM_MAX_ROWS; i++)
        if (sm_match(win->t_history[i], win->t_buffer)) sm_hits[n++] = i;
    return n;
}

// Cosa c'e' sotto il puntatore: un'app, Terminal, Exit, oppure niente.
// Lo usano il disegno, la pressione e il rilascio: la geometria sta qui una
// volta sola, e i tre non possono mai trovarsi in disaccordo.
static int sm_target_at(window_t* win, int mx, int my) {
    int x = win->x, y = win->y;
    int lx = x + SM_LIST_X, ly = y + SM_LIST_Y;

    int n = sm_filter(win);
    int row_w = SM_LIST_W - 8 - ((n > SM_ROWS) ? 14 : 0);
    int list_top = y + SM_ROW_Y;
    if (mx >= lx + 4 && mx < lx + 4 + row_w &&
        my >= list_top && my < list_top + SM_ROWS * SM_ROW_H) {
        int r = (my - list_top) / SM_ROW_H;
        int in_row = (my - list_top - r * SM_ROW_H) < 24;   // non nello stacco fra due righe
        if (in_row && win->t_scroll + r < n) return sm_hits[win->t_scroll + r];
        return SM_PRESS_NONE;
    }

    int rx = x + SM_SYS_X, rw = SM_W - SM_SYS_X - 10;
    if (mx >= rx && mx < rx + rw && my >= ly + 24 && my < ly + 48) return SM_PRESS_TERMINAL;

    if (mx >= x + SM_EXIT_X && mx < x + SM_EXIT_X + SM_EXIT_W &&
        my >= y + SM_EXIT_Y && my < y + SM_EXIT_Y + SM_EXIT_H) return SM_PRESS_EXIT;

    return SM_PRESS_NONE;
}

// ---- disegno ---------------------------------------------------------------
static int sm_isqrt(int n) {
    if (n <= 0) return 0;
    int x = n, y = (x + 1) / 2;
    while (y < x) { x = y; y = (x + n / x) / 2; }
    return x;
}

// Ombra morbida tutto attorno, un po' spostata in basso. Una sola passata:
// l'intensita' cala con la distanza dal bordo arrotondato del pannello.
// Sotto il pannello non serve, perche' il vetro la coprirebbe comunque.
static void sm_shadow(int x, int y, int w, int h, int r) {
    int oy = y + 4;                                   // l'ombra scende di 4 px
    for (int py = oy - SM_SHADOW; py < oy + h + SM_SHADOW; py++) {
        if (py < 0 || py >= fb_height) continue;
        for (int px = x - SM_SHADOW; px < x + w + SM_SHADOW; px++) {
            if (px < 0 || px >= fb_width) continue;
            if (px >= x + r && px < x + w - r && py >= oy + r && py < oy + h - r) {
                px = x + w - r - 1;                   // l'interno si salta
                continue;
            }
            int ddx = 0, ddy = 0;
            if (px < x + r)          ddx = x + r - px;
            else if (px > x + w - 1 - r) ddx = px - (x + w - 1 - r);
            if (py < oy + r)         ddy = oy + r - py;
            else if (py > oy + h - 1 - r) ddy = py - (oy + h - 1 - r);

            int d = (ddx && ddy) ? sm_isqrt(ddx * ddx + ddy * ddy) - r : ddx + ddy - r;
            if (d < 0) d = 0;
            if (d > SM_SHADOW) continue;
            blend_pixel(px, py, 0xFF000000, (SM_SHADOW + 1 - d) * 3);
        }
    }
}

// Vetro smerigliato: lattiginoso, piu' denso in alto, con un riflesso lucido
// e un filo scuro sul bordo. Gli angoli arrotondati si calcolano solo dove
// servono: sui lati dritti la copertura e' semplicemente piena.
static void sm_glass_panel(int x, int y, int w, int h, int r, int milk) {
    int gr = (r > 2) ? r - 2 : 1;                    // raggio del riflesso
    for (int py = y; py < y + h; py++) {
        if (py < 0 || py >= fb_height) continue;
        int dy = py - y;
        int corner_row = (dy < r || dy >= h - r);
        int a_milk = milk - 25 * dy / h;
        int in_gloss = (dy >= 2 && dy < 24);
        int ga = in_gloss ? 70 - 60 * (dy - 2) / 22 : 0;

        for (int px = x; px < x + w; px++) {
            if (px < 0 || px >= fb_width) continue;
            int dx = px - x;
            int corner = corner_row && (dx < r || dx >= w - r);

            int c = corner ? rrect_coverage(px, py, x, y, w, h, r) : 255;
            if (c <= 0) continue;
            int in = corner ? rrect_coverage(px, py, x + 1, y + 1, w - 2, h - 2, r - 1)
                            : ((dx == 0 || dx == w - 1 || dy == 0 || dy == h - 1) ? 0 : 255);

            blend_pixel(px, py, 0xFFF4F7FB, a_milk * c / 255);

            if (in_gloss) {
                int g;
                if (dx < gr + 3 || dx >= w - gr - 3) g = rrect_coverage(px, py, x + 2, y + 2, w - 4, 22, gr);
                else g = 255;
                if (g > 0) blend_pixel(px, py, 0xFFFFFFFF, ga * g / 255);
            }
            if (in < 255) blend_pixel(px, py, 0xFF5A6A80, (255 - in) * c / 255 * 150 / 255);
        }
    }
}

// Pulsante Aqua, disegnato con le stesse chiamate dei pulsanti delle app,
// compreso lo stato premuto. body = 0 usa i colori del tema, come BUTTON_DEFAULT.
static void sm_aqua(int x, int y, int w, int h, uint32_t body, const char* text, uint32_t fg, int pressed) {
    uint32_t veil, rim, body_pressed;
    if (body == 0) {
        body         = active_theme.button_body;
        body_pressed = active_theme.button_body_pressed;
        veil         = active_theme.button_veil;
        rim          = active_theme.button_rim;
    } else {
        body_pressed = gfx_derive_pressed(body);
        veil         = gfx_derive_veil(body);
        rim          = gfx_derive_rim(body);
    }

    uint32_t bg = body, light = 0xFFFFFFFF;
    if (pressed) {
        // Come nei pulsanti delle app: draw_button() rialza il corpo di 4/3
        // quando il light non e' bianco, quindi si pre-scurisce di 3/4
        bg = 0xFF000000
            | ((((body_pressed >> 16) & 0xFF) * 3 / 4) << 16)
            | ((((body_pressed >>  8) & 0xFF) * 3 / 4) <<  8)
            |  ((  body_pressed        & 0xFF) * 3 / 4);
        light = 0xFFC0C0C0;
    }

    gfx_style_aqua(veil, rim);
    draw_button(x, y, w, h, bg, light, 0xFF575757);
    draw_button_rays(x, y, w, h, RAYS_AUTO, pressed ? 55 : 70, active_theme.button_rays);
    gfx_style_reset();

    if (text) {
        int tw = get_prop_string_width_bold(text);
        int tx = x + w / 2 - tw / 2, ty = y + h / 2 - 10;
        draw_prop_string_bold_shadow(text, tx, ty, 3, 0xFF000000, 24);
        draw_prop_string_bold(text, tx, ty, fg);
    }
}

// Una voce: sotto il mouse si accende la pillola Aqua del tema, e mentre e'
// premuta si scurisce. Le voci non ancora disponibili restano grigie.
static void sm_item(int x, int y, int w, int icon, const char* text, int hover, int enabled, int pressed) {
    if (hover && enabled) sm_aqua(x, y, w, 24, 0, 0, 0, pressed);
    draw_icon_16(icon, x + 8, y + 4);
    draw_prop_string_bold(text, x + 32, y + 4, enabled ? 0xFF101418 : 0xFF8A8F96);
}

// Separatore inciso, che sfuma alle estremita'
static void sm_separator(int x, int y, int w) {
    for (int i = 0; i < w; i++) {
        int e = (i < 20) ? i * 255 / 20 : (i > w - 20 ? (w - i) * 255 / 20 : 255);
        blend_pixel(x + i, y,     0xFF7A8698, e * 110 / 255);
        blend_pixel(x + i, y + 1, 0xFFFFFFFF, e * 200 / 255);
    }
}

// Riquadro piatto: solo colore, senza ombre incise. Gli angoli appena
// arrotondati si calcolano solo dove servono.
#define SM_BOX_R 4
static void sm_flat_box(int x, int y, int w, int h, uint32_t color) {
    for (int py = y; py < y + h; py++) {
        int corner_row = (py < y + SM_BOX_R || py >= y + h - SM_BOX_R);
        for (int px = x; px < x + w; px++) {
            int corner = corner_row && (px < x + SM_BOX_R || px >= x + w - SM_BOX_R);
            int c = corner ? rrect_coverage(px, py, x, y, w, h, SM_BOX_R) : 255;
            if (c > 0) blend_pixel(px, py, color, c);
        }
    }
}

static void sm_draw_static(int x, int y) {
    sm_shadow(x, y, SM_W, SM_H, 10);
    sm_glass_panel(x, y, SM_W, SM_H, 10, 225);

    // Intestazione su una lastra del display di vetro
    draw_glass_display(x + 10, y + 10, SM_W - 20, 40, 0xFF20404F);
    draw_os_logo(x + 22, y + 22);
    draw_prop_string_bold(" User", x + 48, y + 22, 0xFFE6F7FF);
    draw_prop_string_bold("V 0.1", x + SM_W - 60, y + 22, 0xFF9FD4E8);

    sm_flat_box(x + SM_LIST_X, y + SM_LIST_Y, SM_LIST_W, SM_LIST_H, 0xFFFFFFFF);
    draw_prop_string_bold("Sistema", x + SM_SYS_X + 8, y + SM_LIST_Y + 2, 0xFF4A5566);
    sm_separator(x + 10, y + SM_H - 46, SM_W - 20);
}

static void sm_draw_dynamic(window_t* win, int mouse_x, int mouse_y) {
    int x = win->x, y = win->y;
    int lx = x + SM_LIST_X, ly = y + SM_LIST_Y;

    // --- Ricerca ---
    draw_textfield(lx + 6, ly + 6, SM_LIST_W - 12, 22, 0xFFFFFFFF);
    int caret_x = lx + 13;
    if (win->t_buffer[0]) {
        draw_prop_string(win->t_buffer, lx + 14, ly + 9, 0xFF101418);
        caret_x = lx + 14 + get_prop_string_width(win->t_buffer) + 1;
    } else {
        draw_prop_string("Cerca...", lx + 14, ly + 9, 0xFF9AA2AC);
    }
    if (blink_counter < 50) draw_rect(caret_x, ly + 10, 1, 14, 0xFF101418);

    // Cosa c'e' sotto il puntatore, e se e' la stessa voce che si sta premendo
    int under = sm_target_at(win, mouse_x, mouse_y);
    int down  = (sm_press != SM_PRESS_NONE && sm_press == under);

    // --- Elenco delle app, filtrato dalla ricerca ---
    int n = sm_filter(win);
    int max_scroll = (n > SM_ROWS) ? n - SM_ROWS : 0;
    if (win->t_scroll > max_scroll) win->t_scroll = max_scroll;
    if (win->t_scroll < 0) win->t_scroll = 0;
    int has_sb = (n > SM_ROWS);
    int row_w = SM_LIST_W - 8 - (has_sb ? 14 : 0);

    if (n == 0) {
        draw_prop_string_bold(win->t_len ? "Nessuna app trovata" : "Nessuna app installata",
                              lx + 12, y + SM_ROW_Y + 4, 0xFF8A8F96);
    }
    for (int r = 0; r < SM_ROWS && win->t_scroll + r < n; r++) {
        int idx = sm_hits[win->t_scroll + r];
        int ry = y + SM_ROW_Y + r * SM_ROW_H;
        sm_item(lx + 4, ry, row_w, 3, win->t_history[idx], under == idx, 1, down && under == idx);
    }

    // Barra di scorrimento, solo se le app non ci stanno tutte
    if (has_sb) {
        int sx = lx + SM_LIST_W - 14, sy = y + SM_ROW_Y, sh = SM_ROWS * SM_ROW_H - 2;
        draw_rect(sx, sy, 8, sh, 0xFFE0E0E0);
        int th = sh * SM_ROWS / n;
        if (th < 12) th = 12;
        int tyy = sy + (sh - th) * win->t_scroll / max_scroll;
        draw_rect(sx + 1, tyy, 6, th, 0xFF909090);
    }

    // --- Voci di sistema ---
    int rx = x + SM_SYS_X, rw = SM_W - SM_SYS_X - 10;
    sm_item(rx, ly + 24, rw, 1, "Terminal", under == SM_PRESS_TERMINAL, 1,
            down && under == SM_PRESS_TERMINAL);
    sm_item(rx, ly + 50, rw, 2, "File Manager", 0, 0, 0);
    sm_item(rx, ly + 76, rw, 4, "Settings", 0, 0, 0);

    // --- Exit, grigio: al primo clic chiede conferma ---
    int armed = (system_ticks < sm_exit_armed_until);
    sm_aqua(x + SM_EXIT_X, y + SM_EXIT_Y, SM_EXIT_W, SM_EXIT_H, 0xFFB9BDC4,
            armed ? "Confirm" : "Exit", 0xFF101418, down && under == SM_PRESS_EXIT);
}

static uint32_t sm_theme_hash(void) {
    const uint8_t* p = (const uint8_t*)&active_theme;
    uint32_t h = 2166136261u;
    for (unsigned i = 0; i < sizeof(active_theme); i++) { h ^= p[i]; h *= 16777619u; }
    return h;
}

static void render_start_menu(window_t* win, int mouse_x, int mouse_y) {
    // Zona dello strato fisso: il menu piu' la sua ombra, tagliata dallo schermo
    int bx = win->x - SM_SHADOW - 2, by = win->y - SM_SHADOW + 2;
    int bw = win->w + 2 * (SM_SHADOW + 2), bh = win->h + 2 * SM_SHADOW + 4;
    if (bx < 0) { bw += bx; bx = 0; }
    if (by < 0) { bh += by; by = 0; }
    if (bx + bw > fb_width)  bw = fb_width - bx;
    if (by + bh > fb_height) bh = fb_height - by;

    uint32_t th = sm_theme_hash();
    int valid = sm_bg_ok && sm_bg_x == bx && sm_bg_y == by &&
                sm_bg_w == bw && sm_bg_h == bh && sm_bg_theme == th;

    if (valid) {
        for (int r = 0; r < bh; r++) {
            uint32_t* dst = &backbuffer[(by + r) * fb_width + bx];
            uint32_t* src = &sm_bg[r * bw];
            for (int c = 0; c < bw; c++) dst[c] = src[c];
        }
    } else {
        sm_draw_static(win->x, win->y);
        // Si memorizza PRIMA di aggiungere cio' che si muove
        if (bw * bh > sm_bg_cap) {
            if (sm_bg) kfree(sm_bg);
            sm_bg = (uint32_t*)kmalloc((size_t)bw * bh * 4);
            sm_bg_cap = sm_bg ? bw * bh : 0;
        }
        if (sm_bg) {
            for (int r = 0; r < bh; r++) {
                uint32_t* src = &backbuffer[(by + r) * fb_width + bx];
                uint32_t* dst = &sm_bg[r * bw];
                for (int c = 0; c < bw; c++) dst[c] = src[c];
            }
            sm_bg_ok = 1;
            sm_bg_x = bx; sm_bg_y = by; sm_bg_w = bw; sm_bg_h = bh;
            sm_bg_theme = th;
        }
    }

    sm_draw_dynamic(win, mouse_x, mouse_y);
}

// ----------------------------------------------------------------------------
// Menu applicazioni (WIN_TYPE_APP_MENU): barra di ricerca, lista scorrevole
// con hover zigrinato e scrollbar.
// ----------------------------------------------------------------------------
static void render_app_menu(window_t* win, int mouse_x, int mouse_y) {
    // Sfondo sottomenu (Nuovo stile Aqua Rounded)
    uint32_t bg_color = 0xFFE1E1E1;
    uint32_t light = 0xFFFFFFFF;
    uint32_t shadow = 0xFF808080;
    uint32_t dark = 0xFF000000;

    // 1. Sfondo e Bordi Arrotondati
    draw_rect(win->x + 2, win->y + 2, win->w - 4, win->h - 4, bg_color);
    draw_rect(win->x + 4, win->y, win->w - 8, 1, light);
    draw_rect(win->x, win->y + 4, 1, win->h - 8, light);
    draw_rect(win->x + 4, win->y + win->h - 1, win->w - 8, 1, dark);
    draw_rect(win->x + win->w - 1, win->y + 4, 1, win->h - 8, dark);

    draw_rect(win->x + 4, win->y + 1, win->w - 8, 1, light);
    draw_rect(win->x + 1, win->y + 4, 1, win->h - 8, light);
    draw_rect(win->x + 4, win->y + win->h - 2, win->w - 8, 1, shadow);
    draw_rect(win->x + win->w - 2, win->y + 4, 1, win->h - 8, shadow);

    // 2. Angoli
    draw_pixel(win->x + 2, win->y + 1, light); draw_pixel(win->x + 3, win->y + 1, light);
    draw_pixel(win->x + 1, win->y + 2, light); draw_pixel(win->x + 1, win->y + 3, light);
    draw_pixel(win->x + win->w - 3, win->y + 1, light); draw_pixel(win->x + win->w - 2, win->y + 2, dark);
    draw_pixel(win->x + 1, win->y + win->h - 3, light); draw_pixel(win->x + 2, win->y + win->h - 2, dark);
    draw_pixel(win->x + win->w - 2, win->y + win->h - 3, dark); draw_pixel(win->x + win->w - 3, win->y + win->h - 2, dark);

    // --- BARRA DI RICERCA ---
    draw_textfield(win->x + 6, win->y + 6, win->w - 12, 24, 0xFFFFFFFF);
    draw_prop_string("Cerca app...", win->x + 12, win->y + 10, 0xFF808080);
    draw_icon_16(6, win->x + win->w - 26, win->y + 10);

    // --- LISTA APPLICAZIONI ---
    int list_start_y = win->y + 34; int item_height = 22;
    int visible_items = (win->h - 38) / item_height;
    int total_items = win->t_len;

    for (int i = 0; i < visible_items; i++) {
        int item_idx = win->t_scroll + i;
        if (item_idx >= total_items) break;
        int item_y = list_start_y + (i * item_height);

        // EFFETTO HOVER MAGIC (ZIGRINATO TONO SU TONO) - Tematizzato Indipendente
        if (mouse_x >= win->x + 4 && mouse_x <= win->x + win->w - 24 && mouse_y >= item_y && mouse_y < item_y + item_height) {
            draw_rect(win->x + 4, item_y, win->w - 28, item_height, active_theme.menu_hover_bg);
            for (int ly = item_y + 1; ly < item_y + item_height; ly += 3) {
                draw_rect(win->x + 4, ly, win->w - 28, 1, active_theme.menu_hover_lines);
            }
            draw_prop_string_bold(win->t_history[item_idx], win->x + 12, item_y + 3, 0xFF000000); // Testo Nero
        } else {
            draw_prop_string_bold(win->t_history[item_idx], win->x + 12, item_y + 3, 0xFF000000);
        }
        draw_rect(win->x + 6, item_y + item_height - 1, win->w - 32, 1, 0xFFC0C0C0);
    }

    // --- SCROLLBAR ---
    int sb_x = win->x + win->w - 22;
    draw_rect(sb_x, list_start_y, 18, win->h - 38, 0xFF808080);
    if (total_items > visible_items) {
        int thmb_h = (visible_items * (win->h - 38)) / total_items;
        int y_off = (win->t_scroll * ((win->h - 38) - thmb_h)) / (total_items - visible_items);
        draw_button(sb_x + 2, list_start_y + y_off, 14, thmb_h, 0xFFC0C0C0, 0xFFFFFFFF, 0xFF000000);
    }
}


// ----------------------------------------------------------------------------
// Segni in stile disegno tecnico attorno al pulsante Main.
// L'idea e' la tavola da disegno: le linee di costruzione si incrociano agli
// angoli e sporgono oltre la figura, perche' il disegnatore le traccia prima
// della forma e non le cancella. E' proprio quella sbavatura a raccontare che
// l'oggetto e' stato costruito, invece che semplicemente disegnato.
// ----------------------------------------------------------------------------
static void draw_tech_marks(int fx0, int fy0, int fx1, int fy1, int ax, int ay) {
    const uint32_t ink = 0xFF000000;
    const int OVER_H = 6;    // sporgenza dei lati orizzontali
    const int OVER_V = 6;    // sporgenza dei lati verticali
    const int SUN_R  = 8;    // raggio da cui l'asse si stacca dal disco

    int fw = (fx1 - fx0) + 1;
    int fh = (fy1 - fy0) + 1;

    // Riquadro: ogni lato oltrepassa l'angolo, cosi' i tratti si incrociano
    draw_rect(fx0 - OVER_H, fy0, fw + OVER_H * 2, 1, ink);
    draw_rect(fx0 - OVER_H, fy1, fw + OVER_H * 2, 1, ink);
    draw_rect(fx0, fy0 - OVER_V, 1, fh + OVER_V * 2, ink);
    draw_rect(fx1, fy0 - OVER_V, 1, fh + OVER_V * 2, ink);

    // Asse di simmetria del logo: si interrompe sul disco invece di
    // attraversarlo, e sporge oltre il riquadro come una linea di riporto.
    int top_y = fy0 - OVER_V;
    int bot_y = fy1 + OVER_V;
    if (ay - SUN_R > top_y) draw_rect(ax, top_y, 1, (ay - SUN_R) - top_y, ink);
    if (bot_y > ay + SUN_R) draw_rect(ax, ay + SUN_R, 1, bot_y - (ay + SUN_R), ink);

    // Due riferimenti di quota che scendono dal lato inferiore
    draw_rect(fx0 + 9, fy1 + 2, 1, 3, ink);
    draw_rect(fx1 - 9, fy1 + 2, 1, 3, ink);
}


// ----------------------------------------------------------------------------
// Mensola del testo. Un sostegno verticale sale dal bordo dello schermo,
// attraversa la sorgente dei raggi e regge il ripiano su cui la scritta
// appoggia. Ripiano e sostegno sono profili grigi con l'anima chiara al
// centro: lo stesso linguaggio delle cornici tecniche del pulsante Main, ma
// in volume invece che a filo.
// Il fondo bianco si dissolve verso l'alto E ai lati, altrimenti un rettangolo
// pieno largo quanto il pulsante spegnerebbe il ventaglio di luce.
// ----------------------------------------------------------------------------
static void draw_text_shelf(int bx, int by, int bw) {
    const uint32_t edge = 0xFF6E6E6E;    // grigio dei bordi del profilo
    const uint32_t core = 0xFFC8C8C8;    // grigio chiaro dell'anima
    const int TH     = 3;                // spessore dei profili
    const int FADE_Y = 9;                // dissolvenza verso l'alto
    const int FADE_X = 14;               // dissolvenza ai fianchi

    if (bw < 8) return;

    // Quota del ripiano, allineata in alto al rettangolo bianco del Main.
    // Se la barra e' ancorata in basso resta comunque dentro lo schermo.
    int uy = by + 22;
    if (uy + TH > fb_height) uy = fb_height - TH;

    // La mensola si calcola per prima: i suoi estremi sono anche i limiti
    // entro cui il fondo bianco deve nascere e spegnersi.
    int sx0 = bx + FADE_X - 4;
    int sx1 = bx + bw - 1 - (FADE_X - 4);
    if (sx1 - sx0 < 6) { sx0 = bx + 1; sx1 = bx + bw - 2; }   // pulsanti stretti
    if (sx1 - sx0 < 2) return;
    int sw = sx1 - sx0 + 1;

    // Fondo bianco sotto la scritta, contenuto nella larghezza del ripiano
    int ry0 = by - 1;
    for (int py = ry0; py < uy; py++) {
        int rv = (py - ry0 >= FADE_Y) ? 255 : (py - ry0) * 255 / FADE_Y;
        int av = smooth255(rv);
        if (av <= 0) continue;

        for (int px = sx0; px <= sx1; px++) {
            int dl = px - sx0;
            int dr = sx1 - px;
            int hf = (dl < dr) ? dl : dr;
            int ah = smooth255((hf >= FADE_X) ? 255 : hf * 255 / FADE_X);
            if (ah <= 0) continue;

            blend_pixel(px, py, 0xFFFFFFFF, av * ah / 255);
        }
    }

    draw_rect(sx0 + 1, uy,     sw - 2, 1, edge);
    draw_rect(sx0 + 1, uy + 1, sw - 2, 1, core);
    draw_rect(sx0 + 1, uy + 2, sw - 2, 1, edge);

    // Testate: un pixel pieno al centro e due mezzi toni sopra e sotto. Su un
    // profilo alto tre pixel e' tutto cio' che serve per smussare l'angolo, e
    // chiude anche i fianchi che prima restavano tagliati sul grigio chiaro.
    draw_pixel(sx0, uy + 1, edge);
    draw_pixel(sx1, uy + 1, edge);
    blend_pixel(sx0, uy,     edge, 115);
    blend_pixel(sx0, uy + 2, edge, 115);
    blend_pixel(sx1, uy,     edge, 115);
    blend_pixel(sx1, uy + 2, edge, 115);

    // Gambo: i fianchi partono sotto il ripiano, mentre l'anima chiara risale
    // fino a quella del ripiano. Cosi' i due grigi chiari si saldano invece di
    // restare divisi dal bordo inferiore, e il sostegno sembra un pezzo solo.
    int cx = bx + bw / 2;
    int vy = uy + TH;
    if (fb_height > vy) {
        draw_rect(cx - 1, vy, 1, fb_height - vy, edge);
        draw_rect(cx + 1, vy, 1, fb_height - vy, edge);
        draw_rect(cx,     uy + 1, 1, fb_height - (uy + 1), core);
    }
}

// ----------------------------------------------------------------------------
// Media di un rettangolo del backbuffer: e' cio' che una superficie riflettente
// "vede" in quella direzione. Va chiamato PRIMA di disegnarci sopra, altrimenti
// si campionerebbe se stessi e il colore si avvolgerebbe su se' stesso.
// ----------------------------------------------------------------------------
static void sample_ambient(int x, int y, int w, int h, int* r, int* g, int* b) {
    int sr = 0, sg = 0, sb = 0, n = 0;

    for (int py = y; py < y + h; py++) {
        if (py < 0 || py >= fb_height) continue;
        for (int px = x; px < x + w; px++) {
            if (px < 0 || px >= fb_width) continue;
            uint32_t c = backbuffer[py * fb_width + px];
            sr += (c >> 16) & 0xFF;
            sg += (c >> 8) & 0xFF;
            sb += c & 0xFF;
            n++;
        }
    }
    if (n == 0) { *r = 200; *g = 200; *b = 200; return; }
    *r = sr / n; *g = sg / n; *b = sb / n;
}
// ----------------------------------------------------------------------------
// Sagoma della visiera: spalle superiori appena raccordate e fondo a mezza
// ellisse. Serve una forma dedicata perche' un rettangolo arrotondato, per
// quanto tondo agli angoli, ha comunque il bordo inferiore dritto al centro.
// Supercampionata 4x4 come il resto dell'interfaccia; solo interi.
// ----------------------------------------------------------------------------
// split_y: riga in cui il rettangolo cede all'arco. E' un parametro a se'
// perche' altrimenti dipenderebbe dall'altezza, e allungare la maschera verso
// l'alto ne cambierebbe anche il mento.
static int visor_cov(int px, int py, int x, int y, int w, int h, int split_y) {
    int x0 = x * 8, x1 = (x + w) * 8;
    int y0 = y * 8;
    int cx8 = x0 + w * 4;
    int rx  = w * 4;                    // semilarghezza
    int topr = 24;                      // raccordo delle spalle: 3 px
    int split = split_y * 8;            // dove il rettangolo cede all'arco
    int ryb = (y + h) * 8 - split;      // semiasse verticale dell'arco
    if (rx < 1 || ryb < 1) return 0;

    int hits = 0;
    for (int sy = 0; sy < 4; sy++) {
        int fy = py * 8 + sy * 2 + 1;
        if (fy < y0) continue;

        for (int sx = 0; sx < 4; sx++) {
            int fx = px * 8 + sx * 2 + 1;
            if (fx < x0 || fx > x1) continue;

            if (fy <= split) {
                // Parte alta: rettangolo con le due spalle raccordate
                int dy = fy - (y0 + topr);
                if (dy < 0) {
                    int dl = fx - (x0 + topr);
                    int dr = fx - (x1 - topr);
                    if (dl < 0) { if (dl * dl + dy * dy > topr * topr) continue; }
                    else if (dr > 0) { if (dr * dr + dy * dy > topr * topr) continue; }
                }
                hits++;
            } else {
                // Parte bassa: quarto di ellisse, cosi' il mento e' curvo
                long a = (long)(fx - cx8) * ryb;
                long b = (long)(fy - split) * rx;
                long rr = (long)rx * ryb;
                if (a * a + b * b > rr * rr) continue;
                hits++;
            }
        }
    }
    return hits * 255 / 16;
}

void draw_mascot(int bx2, int by2);
static void draw_tray(int cx, int base, uint32_t fill, uint32_t rim);
static void draw_tray_particles(int cx, int base, int seed);


// ============================================================================
// MEMORIA DELLA TASKBAR
// Stesso principio delle finestre, con due differenze. Il vetro e' traslucido,
// quindi la firma include anche lo sfondo che gli sta dietro. E il mouse conta
// solo quando e' sopra la barra: e' li' che stanno Main e Info.
// Restano vivi a ogni fotogramma solo particelle e mascotte di casa.
// ============================================================================
#define TB_CACHE_ROWS 64
static uint32_t* tb_cache = 0;
static int      tb_cache_ok = 0;
static uint32_t tb_cache_sig = 0;
static int      tb_cx = 0, tb_cy = 0, tb_cw = 0, tb_ch = 0;

// Definite piu' sotto, insieme alla memoria delle finestre
static uint32_t sig_u32(uint32_t h, uint32_t v);
static uint32_t sig_bytes(uint32_t h, const void* p, int n);

static uint32_t taskbar_signature(int x, int y, int w, int h,
                                  int mouse_x, int mouse_y, int is_clicking) {
    uint32_t s = 2166136261u;
    s = sig_u32(s, (uint32_t)x);
    s = sig_u32(s, (uint32_t)y);
    s = sig_u32(s, (uint32_t)w);
    s = sig_u32(s, (uint32_t)h);
    s = sig_u32(s, (uint32_t)taskbar_y_offset);

    // Cio' che sta dietro il vetro, letto PRIMA di disegnare la barra
    for (int r = 0; r < h; r++) {
        const uint32_t* row = &backbuffer[(y + r) * fb_width + x];
        for (int c = 0; c < w; c++) s = sig_u32(s, row[c]);
    }

    // Pulsanti delle applicazioni: quali ci sono, come si chiamano, chi e' attivo
    for (int i = 0; i < MAX_WINDOWS; i++) {
        s = sig_u32(s, (uint32_t)windows[i].active);
        if (!windows[i].active) continue;
        s = sig_u32(s, (uint32_t)windows[i].is_minimized);
        s = sig_u32(s, (uint32_t)windows[i].type);
        s = sig_bytes(s, windows[i].title, sizeof(windows[i].title));
    }
    s = sig_u32(s, (uint32_t)global_active_win);

    // Orologio: la barra si ridisegna una volta al secondo
    s = sig_u32(s, rtc_hour);
    s = sig_u32(s, rtc_minute);
    s = sig_u32(s, rtc_second);

    s = sig_bytes(s, &active_theme, sizeof(active_theme));

    // Il mouse conta solo sopra la barra: altrove non la tocca
    if (mouse_x >= x && mouse_x < x + w && mouse_y >= y && mouse_y < y + h) {
        s = sig_u32(s, (uint32_t)mouse_x);
        s = sig_u32(s, (uint32_t)mouse_y);
        s = sig_u32(s, (uint32_t)is_clicking);
    }
    return s;
}

// Cio' che resta vivo sopra la barra: particelle e mascotte di casa.
// Anche il ritorno a casa della mascotte sta qui, perche' deve avvenire a ogni
// fotogramma, compresi quelli in cui la barra viene solo ricopiata.
static void taskbar_live_layer(void) {
    if (mascot_host >= 0 &&
        (mascot_host >= MAX_WINDOWS || !windows[mascot_host].active))
        mascot_host = -1;

    draw_tray_particles(home_tray_cx, home_tray_base, 0);

    if (mascot_where() == 1 && !mascot_drag)
        draw_mascot(mascot_home_x, mascot_home_y);
}


// ----------------------------------------------------------------------------
// Taskbar animata: sfondo, pulsante Main, orologio analogico, tasto Info,
// bottoni delle app e separatore con simbolo dell'integrale.
// ----------------------------------------------------------------------------
static void render_taskbar(int mouse_x, int mouse_y, int is_clicking) {
    int tb_y = taskbar_content_y();

    // 1. Calcolo Dinamico del Target di Larghezza
    int max_tb_w = fb_width - 20; // Dimensione massima (larga quasi tutto lo schermo)
    int min_tb_w = 220;           // Dimensione minima di avvio (Menu + Info + Orologio)
    
    int valid_wins[MAX_WINDOWS];
    int valid_count = get_taskbar_windows(valid_wins);
    
    // Ogni applicazione aperta aggiunge 165 pixel di respiro alla barra
    taskbar_target_w = min_tb_w + (valid_count * 165);
    if (taskbar_target_w > max_tb_w) taskbar_target_w = max_tb_w;

    // 2. Geometria Attuale della Barra (in base allo step di animazione)
    int tb_w = taskbar_w;
    int tb_x = (fb_width - tb_w) / 2; // Centratura orizzontale perfetta e automatica


    // --- MEMORIA DELLA TASKBAR: se nulla e' cambiato, si ricopia e basta ---
    // La zona parte 6px sopra il vetro: da ancorata, raggi e segni tecnici
    // salgono di qualche pixel oltre il suo bordo.
    int trx0 = tb_x - 8, trx1 = tb_x + tb_w + 8;
    int try0 = taskbar_panel_top() - 6, try1 = fb_height;
    if (trx0 < 0) trx0 = 0;
    if (trx1 > fb_width) trx1 = fb_width;
    if (try0 < 0) try0 = 0;
    int trw = trx1 - trx0, trh = try1 - try0;

    int tb_use_cache = (trw > 0 && trh > 0 && trh <= TB_CACHE_ROWS);
    if (tb_use_cache && !tb_cache)
        tb_cache = (uint32_t*)kmalloc((size_t)fb_width * TB_CACHE_ROWS * 4);
    if (!tb_cache) tb_use_cache = 0;

    uint32_t tsig = 0;
    if (tb_use_cache) {
        tsig = taskbar_signature(trx0, try0, trw, trh, mouse_x, mouse_y, is_clicking);
        if (tb_cache_ok && tb_cache_sig == tsig &&
            tb_cx == trx0 && tb_cy == try0 && tb_cw == trw && tb_ch == trh) {
            for (int r = 0; r < trh; r++) {
                uint32_t* dst = &backbuffer[(try0 + r) * fb_width + trx0];
                uint32_t* src = &tb_cache[r * trw];
                for (int c = 0; c < trw; c++) dst[c] = src[c];
            }
            taskbar_live_layer();
            return;
        }
    }

    // Pannello in vetro: parte 3px sopra gli elementi e arriva al bordo
    // inferiore dello schermo, sotto il punto dove finiva la vecchia barra.
    draw_glass_panel(tb_x, taskbar_panel_top(), tb_w, taskbar_panel_h(), active_theme.taskbar_glow);
    // ========================================================================
    // PULSANTE MENU: FASCIO DI LUCE (Ancorato al bordo sinistro dinamico)
    // ========================================================================
    //uint32_t bg_current = active_theme.desktop_bg;    // Tematizzato: Sfuma nel colore del desktop
    uint32_t neon_base  = active_theme.primary_color; // Tematizzato: Colore del fascio al click
    uint32_t beam_white = 0xFFFFFFFF; 
    
    int start_x = tb_x + 7; // Segue fedelmente l'espansione sinistra della barra!
    int start_w = 80;

    int is_pressed = (mouse_x >= start_x && mouse_x <= start_x + start_w && mouse_y >= tb_y && mouse_y <= tb_y + 26 && is_clicking);
    uint32_t current_mid_color = is_pressed ? neon_base : beam_white;

    // Alone del pulsante Main: sfuma in modo omogeneo su tutti e quattro i lati.
    // L'attenuazione orizzontale e verticale si MOLTIPLICANO invece di prendere
    // la minore: cosi' agli angoli non si formano le pieghe diagonali che
    // tradirebbero un alone rettangolare.
    int bm_x = start_x,  bm_w = start_w;
    int bm_y = tb_y - 1, bm_h = 30;
    const int FEATHER = 8;

    for (int py = bm_y; py < bm_y + bm_h; py++) {
        int dt = py - bm_y;
        int db = (bm_y + bm_h - 1) - py;
        int vf = (dt < db) ? dt : db;
        int av = smooth255((vf >= FEATHER) ? 255 : vf * 255 / FEATHER);
        if (av <= 0) continue;

        for (int px = bm_x; px < bm_x + bm_w; px++) {
            int dl = px - bm_x;
            int dr = (bm_x + bm_w - 1) - px;
            int hf = (dl < dr) ? dl : dr;
            int ah = smooth255((hf >= FEATHER) ? 255 : hf * 255 / FEATHER);
            if (ah <= 0) continue;

            int a = ah * av / 255;
            blend_pixel(px, py, beam_white, a * 235 / 255);
            if (is_pressed) blend_pixel(px, py, current_mid_color, a * 110 / 255);
        }
    }

    // --- CENTRATURA PERFETTA LOGO E TESTO "Main" ---
    int main_str_w = get_prop_string_width_bold("Main");
    int combined_w = 24 + 5 + main_str_w; 
    int content_start_x = start_x + (start_w - combined_w) / 2;


    // Segni di costruzione attorno all'alone. L'asse verticale cade sul centro
    // del disco del logo, che sta a meta' fra due colonne: da qui il +15.
    draw_tech_marks(bm_x + 5, bm_y + 4, bm_x + bm_w - 6, bm_y + bm_h - 5, content_start_x + 15, tb_y + 13);

    draw_os_logo(content_start_x, tb_y + 6); 
    draw_prop_string_bold("Main", content_start_x + 29, tb_y + 5, 0xFF000000);

    // ========================================================================
    // OROLOGIO ANALOGICO E INFO (Ancorati a destra, Layout Rettangolare)
    // ========================================================================
    read_rtc();
    static const int clk_dx[60] = {
        0, 104, 207, 309, 406, 500, 587, 669, 743, 809, 866, 913, 951, 978, 994,
        1000, 994, 978, 951, 913, 866, 809, 743, 669, 587, 500, 406, 309, 207, 104,
        0, -104, -207, -309, -406, -500, -587, -669, -743, -809, -866, -913, -951, -978, -994,
        -1000, -994, -978, -951, -913, -866, -809, -743, -669, -587, -500, -406, -309, -207, -104
    };
    static const int clk_dy[60] = {
        -1000, -994, -978, -951, -913, -866, -809, -743, -669, -587, -500, -406, -309, -207, -104,
        0, 104, 207, 309, 406, 500, 587, 669, 743, 809, 866, 913, 951, 978, 994,
        1000, 994, 978, 951, 913, 866, 809, 743, 669, 587, 500, 406, 309, 207, 104,
        0, -104, -207, -309, -406, -500, -587, -669, -743, -809, -866, -913, -951, -978, -994
    };

    // L'orologio mantiene l'ingombro di 26x26 pixel, posizionato a 33px dal margine destro
    int clock_w = 26;
    int clock_h = 26;
    int clock_x = (tb_x + tb_w) - 33; 
    int clock_y = tb_y;          
    int cx = clock_x + 13; // Centro per le lancette      
    int cy = clock_y + 13;       

    // 1. Sfondo Orologio: Bordo nero esterno perfettametne squadrato e interno bianco
    draw_rect(clock_x, clock_y, clock_w, clock_h, 0xFF000000);
    draw_rect(clock_x + 1, clock_y + 1, clock_w - 2, clock_h - 2, 0xFFFFFFFF);

    // Calcolo e Disegno Lancette (Invariato)
    int s_idx = rtc_second % 60;
    int m_idx = rtc_minute % 60;
    int h_idx = ((rtc_hour % 12) * 5) + (rtc_minute / 12); 

    for (int l = 0; l <= 6; l++) {
        int hx = cx + (clk_dx[h_idx] * l) / 1000; int hy = cy + (clk_dy[h_idx] * l) / 1000;
        draw_pixel(hx, hy, 0xFF000000); draw_pixel(hx + 1, hy, 0xFF000000); 
        draw_pixel(hx, hy + 1, 0xFF000000); draw_pixel(hx + 1, hy + 1, 0xFF000000);
    }
    for (int l = 0; l <= 10; l++) {
        int mx = cx + (clk_dx[m_idx] * l) / 1000; int my = cy + (clk_dy[m_idx] * l) / 1000;
        draw_pixel(mx, my, 0xFF000000); if (l < 6) draw_pixel(mx + 1, my, 0xFF000000); 
    }
    for (int l = 0; l <= 12; l++) {
        int sx = cx + (clk_dx[s_idx] * l) / 1000; int sy = cy + (clk_dy[s_idx] * l) / 1000;
        draw_pixel(sx, sy, 0xFFFF0000); 
    }
    draw_pixel(cx, cy, 0xFFFF0000); draw_pixel(cx + 1, cy, 0xFFFF0000);

    // ========================================================================
    // PULSANTE "INFO" (Gemello Rettangolare a sinistra dell'orologio)
    // ========================================================================
    int info_w = 26;
    int info_h = 26;
    int info_x = clock_x - info_w - 10; 
    int info_y = tb_y;
    int icx = info_x + 13; // Centro per la 'i'
    int icy = info_y + 13; 

    // Hitbox e Colore Dinamico
    uint32_t info_bg = 0xFF0066CC; 
    if (mouse_x >= info_x && mouse_x <= info_x + info_w && mouse_y >= info_y && mouse_y <= info_y + info_h && is_clicking) {
        info_bg = 0xFF004499; 
    }
    
    // Sfondo Info: Bordo nero squadrato e interno blu dinamico
    draw_rect(info_x, info_y, info_w, info_h, 0xFF000000);
    draw_rect(info_x + 1, info_y + 1, info_w - 2, info_h - 2, info_bg);

    // Disegno della 'i' corsiva bianca (perfettamente centrata)
    draw_rect(icx + 2, icy - 6, 2, 2, 0xFFFFFFFF); 
    draw_rect(icx + 1, icy - 2, 2, 2, 0xFFFFFFFF);
    draw_rect(icx + 0, icy + 0, 2, 2, 0xFFFFFFFF);
    draw_rect(icx - 1, icy + 2, 2, 2, 0xFFFFFFFF);
    draw_rect(icx - 2, icy + 4, 2, 3, 0xFFFFFFFF);

    // ========================================================================
    // AREA PULSANTI DELLE APP (Adattiva con Centratura e Separatore)
    // ========================================================================
    int tb_start_x = start_x + start_w + 8; // Inizia subito dopo il Menu
    
    // AGGIORNAMENTO: L'area per i bottoni termina prima del limite massimo del separatore!
    // (info_x - 15 è il centro del separatore, togliamo altri 12px per non toccarne il bordo)
    int tb_end_x = (info_x - 15) - 12;      
    int tb_area_w = tb_end_x - tb_start_x;

    int actual_btn_w_calc = 0;

    if (valid_count > 0 && tb_area_w > 10) {
        int btn_w = tb_area_w / valid_count;
        if (btn_w > 160) btn_w = 160; 
        actual_btn_w_calc = btn_w;
        
        for(int i = 0; i < valid_count; i++) {
            int w_id = valid_wins[i];
            int bx = tb_start_x + (i * btn_w);
            int by = tb_y; 
            
            if (btn_w > 30) {
                int is_active_win = (global_active_win == w_id && !windows[w_id].is_minimized);
                
                

            

                // Centratura dinamica Icona e Testo
                int actual_btn_w = btn_w - 4;
                int max_text_px = actual_btn_w - 16;  
                
                char short_title[32]; 
                int t_len = 0;
                int current_str_px = 0;
                
                while(windows[w_id].title[t_len] != '\0' && t_len < 31) {
                    unsigned char uc = (unsigned char)windows[w_id].title[t_len];
                    int char_w = FONT_HAS(uc) ? (uc == ' ' ? 6 : font_prop[uc].width + 2) : 9;
                    
                    if (current_str_px + char_w > max_text_px) break;
                    short_title[t_len] = windows[w_id].title[t_len];
                    current_str_px += char_w;
                    t_len++;
                }
                short_title[t_len] = '\0';
                
                int content_start_x = bx + (actual_btn_w - current_str_px) / 2;

                // Puro testo su un ventaglio di luce. La sorgente sta sul bordo
                // inferiore dello schermo, non del pulsante: e' quella corsa in
                // piu' che permette ai raggi di aprirsi prima di arrivare al testo.
                int gl_y = by - 4;
                draw_rising_rays(bx, gl_y, actual_btn_w, fb_height - gl_y, RAYS_AUTO,
                                 is_active_win ? 255 : 225,
                                 is_active_win ? 0xFFFFC400 : 0xFFFFFFFF,
                                 is_active_win ? 0xFFFFFFFF : 0);

                // Mensola: sostegno, ripiano e fondo chiaro del testo
                draw_text_shelf(bx, by, actual_btn_w);
                
                if (t_len > 0) {
                    int tb_ty = by + 3;   // alzato: il testo poggia sul ripiano
                    

                    // Ombra nera appena accennata. Due passate a distanze
                    // diverse allargano la sfumatura: una sola darebbe un
                    // contorno netto invece di una diffusione morbida.
                    draw_prop_string_bold_shadow(short_title, content_start_x, tb_ty, 4, 0xFF000000, 26);
                    draw_prop_string_bold_shadow(short_title, content_start_x, tb_ty, 2, 0xFF000000, 34);
                    draw_prop_string_bold(short_title, content_start_x, tb_ty, 0xFF000000);
                }
            }
        }
    }

    // ========================================================================
    // SEPARATORE MATEMATICO CON SIMBOLO DELL'INTEGRALE (Con Limite Anti-Collisione)
    // ========================================================================
    // 1. Calcoliamo l'ingombro teorico dei pulsanti delle app da sinistra.
    int apps_occupied_end = tb_start_x + (valid_count * actual_btn_w_calc);
    
    // 2. Calcoliamo la posizione ideale del separatore (a destra delle app con respiro).
    int sep_cx = apps_occupied_end + 24;
    int sep_cy = tb_y + 13;

    // 3. CAP DI SICUREZZA (Anti-Sovrapposizione):
    // Il separatore non deve MAI invadere lo spazio vitale dell'icona Info.
    // Fissiamo il limite massimo a 15 pixel a sinistra del tasto Info.
    int max_sep_x = info_x - 15;
    if (sep_cx > max_sep_x) {
        sep_cx = max_sep_x;
    }

    // Il vassoio resta dove era; e' la mascotte a sollevarsi di due pixel,
    // cosi' sembra posata invece che incollata.
    home_tray_cx   = sep_cx;
    home_tray_base = sep_cy - MASCOT_H / 2 + MASCOT_H + 1;
    mascot_home_x  = sep_cx - MASCOT_W / 2;
    mascot_home_y  = sep_cy - MASCOT_H / 2 - 2;

        // Il vassoio e' statico: finisce in memoria insieme al resto
    draw_tray(home_tray_cx, home_tray_base, 0xFFC8C8C8, 0xFF6E6E6E);

    // La barra e' disegnata: la memorizziamo, prima di aggiungere cio' che e' vivo
    if (tb_use_cache) {
        for (int r = 0; r < trh; r++) {
            uint32_t* src = &backbuffer[(try0 + r) * fb_width + trx0];
            uint32_t* dst = &tb_cache[r * trw];
            for (int c = 0; c < trw; c++) dst[c] = src[c];
        }
        tb_cache_sig = tsig;
        tb_cx = trx0; tb_cy = try0; tb_cw = trw; tb_ch = trh;
        tb_cache_ok = 1;
    }

    taskbar_live_layer();
}

// ----------------------------------------------------------------------------
// MASCOTTE: una capsula metallica con gli occhi che si scambiano il colore a
// ogni secondo. Prende la posizione dall'esterno perche' puo' vivere sulla
// taskbar, sul vassoio di una finestra o appesa al cursore.
// ----------------------------------------------------------------------------
void draw_mascot(int bx2, int by2) {
    const uint32_t bot_edge = 0xFF6E6E6E;
    const uint32_t bot_core = 0xFFC8C8C8;

    int bw2 = MASCOT_W, bh2 = MASCOT_H;
    int sep_cx = bx2 + bw2 / 2;
    int brad = bw2 / 2;                 // meta' larghezza: calotte semicircolari

    
    // Profilo speculare lungo la larghezza. La differenza fra metallo e plastica
    // grigia sta tutta qui: la plastica schiarisce e scurisce una volta sola,
    // il metallo ha una luce viva vicino alla sorgente, un cuore scuro e una
    // SECONDA luce sul bordo opposto. Sono quei due picchi a farlo leggere come cromo.
    static const short metal[13] = {
        148, 196, 238, 255, 214, 158, 116,  96, 104, 132, 178, 222, 172
    };

    // Cosa riflette. Il centro della capsula prende cio' che le sta
    // direttamente sotto; i fianchi prendono la meta' corrispondente della
    // sua impronta allargata di un margine, cosi' la mascotte comincia a
    // tingersi gia' mentre si avvicina a qualcosa, prima di sovrapporvisi.
    // Il campionamento avviene PRIMA di disegnare corpo e alone, quindi legge
    // la scena pulita e non se stessa.
    const int REFL_M = 10;                       // margine di avvicinamento
    int lr, lg, lb, rr, rg, rb, ur, ug, ub;
    sample_ambient(bx2 - REFL_M,     by2 - REFL_M / 2,
                   REFL_M + bw2 / 2, bh2 + REFL_M,          &lr, &lg, &lb);
    sample_ambient(bx2 + bw2 / 2,    by2 - REFL_M / 2,
                   bw2 / 2 + REFL_M + 1, bh2 + REFL_M,      &rr, &rg, &rb);
    sample_ambient(bx2,              by2,
                   bw2,              bh2,                   &ur, &ug, &ub);

    // Bagliore dietro la mascotte. Sommando le coperture di capsule via via
    // piu' grandi si ottiene un alone che segue la sagoma alla stessa distanza
    // su ogni lato, invece di un cerchio che sui fianchi resterebbe largo e
    // sulle calotte stretto.
    // Va steso DOPO il campionamento dell'ambiente e PRIMA del corpo: altrimenti
    // la mascotte rifletterebbe il proprio alone e si schiarirebbe da sola.
    {
        const int GSPREAD = 6;    // quanto si allarga
        const int GMAX = 150;     // intensita' a ridosso del bordo

        for (int py = by2 - GSPREAD; py < by2 + bh2 + GSPREAD; py++) {
            for (int px = bx2 - GSPREAD; px < bx2 + bw2 + GSPREAD; px++) {
                // Sotto la sagoma non serve: la coprira' il corpo
                if (rrect_coverage(px, py, bx2, by2, bw2, bh2, brad) >= 250) continue;

                int acc = 0;
                for (int k = 1; k <= GSPREAD; k++)
                    acc += rrect_coverage(px, py, bx2 - k, by2 - k,
                                          bw2 + 2 * k, bh2 + 2 * k, brad + k);

                int s = acc / GSPREAD;
                if (s <= 0) continue;
                s = s * s / 255;                  // caduta quadratica
                int a = GMAX * s / 255;
                if (a > 1) blend_pixel(px, py, 0xFFB98CF5, a);
            }
        }
    }

    for (int py = by2; py < by2 + bh2; py++) {
        // Alle estremita' le calotte curvano via dalla sorgente e si spengono
        int t = (py - by2) * 255 / (bh2 - 1);
        int vshade = 255 - ((t < 40) ? (40 - t) * 2 : 0)
                         - ((t > 200) ? (t - 200) * 2 : 0);
        if (vshade < 150) vshade = 150;

        for (int px = bx2; px < bx2 + bw2; px++) {
            int cov = rrect_coverage(px, py, bx2, by2, bw2, bh2, brad);
            if (cov <= 0) continue;

            int inr = rrect_coverage(px, py, bx2 + 1, by2 + 1, bw2 - 2, bh2 - 2, brad - 1);

            int col = px - bx2;
            if (col < 0) col = 0;
            if (col > 12) col = 12;

            // Peso delle tre direzioni per questa colonna
            int wl = 0, wr = 0;
            if (col <= 3)      wl = (4 - col) * 64;
            else if (col >= 9) wr = (col - 8) * 64;
            int wu = 256 - wl - wr;

            int ar = (lr * wl + rr * wr + ur * wu) / 256;
            int ag = (lg * wl + rg * wr + ug * wu) / 256;
            int ab = (lb * wl + rb * wr + ub * wu) / 256;

            // Meta' riflesso, meta' grigio proprio: un metallo che riflettesse
            // al cento per cento perderebbe la sua identita' e cambierebbe
            // colore a ogni finestra che passa.
            int base = (bot_core >> 16) & 0xFF;
            ar = (ar + base) / 2;
            ag = (ag + base) / 2;
            ab = (ab + base) / 2;

            int lum = metal[col] * vshade / 255;
            int cr = ar * lum / 178;
            int cg = ag * lum / 178;
            int cb = ab * lum / 178;

            if (cr < 16) cr = 16;
            if (cr > 255) cr = 255;
            if (cg < 16) cg = 16;
            if (cg > 255) cg = 255;
            if (cb < 16) cb = 16;
            if (cb > 255) cb = 255;

            uint32_t body = 0xFF000000 | (cr << 16) | (cg << 8) | cb;

            blend_pixel(px, py, body, cov * inr / 255);
            if (inr < 255) blend_pixel(px, py, bot_edge, cov * (255 - inr) / 255);
        }
    }

    // Scintilla speculare: un solo pixel pieno sulla colonna piu' illuminata,
    // in alto. E' minuscola ma e' cio' che dice all'occhio "questa superficie
    // e' lucida", perche' solo una superficie levigata produce un punto netto.
    blend_pixel(bx2 + 3, by2 + 3, 0xFFFFFFFF, 200);
    blend_pixel(bx2 + 3, by2 + 4, 0xFFFFFFFF, 110);
    blend_pixel(bx2 + 4, by2 + 3, 0xFFFFFFFF, 90);


    // La maschera parte sopra la capsula e va larga quanto la sua parte
    // interna: il ritaglio piu' sotto le da' da solo la curvatura della
    // calotta, quindi copre tutto l'interno senza mai toccare il bordo.
    // Solo l'arco inferiore resta una forma sua.
    // Larghezza e mento identici a prima: ai fianchi ci pensa gia' il ritaglio
    // dentro la capsula. Cambia solo il bordo alto, che ora parte sopra il
    // corpo e prende da solo la curvatura della calotta.
    int vis_x = sep_cx - 8,  vis_w = 17;
    int vis_y = by2 - 2,     vis_h = 14;
    int vis_split = by2 + 7;            // stessa quota di prima: mento invariato

    for (int py = vis_y - 1; py < vis_y + vis_h + 1; py++) {
        for (int px = vis_x - 1; px < vis_x + vis_w + 1; px++) {
            // Ritagliata dentro la sagoma: sul bordo della capsula si ferma
            int in = rrect_coverage(px, py, bx2 + 1, by2 + 1, bw2 - 2, bh2 - 2, brad - 1);
            if (in <= 0) continue;

            int cv = visor_cov(px, py, vis_x, vis_y, vis_w, vis_h, vis_split);

            // Filo nero: e' la sagoma allargata di un pixel MENO la sagoma
            // vera. Piu' sottile di cosi' non si puo' fare, e resta morbido
            // perche' nasce dalla differenza di due coperture antialiasate.
            int out = visor_cov(px, py, vis_x - 1, vis_y - 1, vis_w + 2, vis_h + 2, vis_split + 1);
            if (out > cv) blend_pixel(px, py, 0xFF000000, (out - cv) * in / 255);

            //if (cv > 0) blend_pixel(px, py, 0xFFFFFFFF, cv * in / 255);
            if (cv > 0) {
                // Stesso profilo speculare del corpo, ma appena accennato:
                // e' la curva della riflessione a dire di che materiale e'
                // fatta una superficie, non il suo colore. Applicandola anche
                // qui, bianco e grigio diventano lo stesso metallo in due
                // finiture invece di due materiali diversi.
                int mc = px - bx2;
                if (mc < 0) mc = 0;
                if (mc > 12) mc = 12;

                // L'escursione va aperta sul serio: su un fondo quasi bianco
                // l'occhio distingue le differenze molto peggio che sui toni
                // medi, e trenta livelli spalmati su undici colonne finiscono
                // sotto la soglia di percezione.
                const int MASK_LO = 190;
                int sh = MASK_LO + (metal[mc] - 96) * (255 - MASK_LO) / 159;
                if (sh < 0) sh = 0;
                if (sh > 255) sh = 255;

                // La maschera raccoglie anche l'ambiente, come il corpo. Senza
                // questo, quando la mascotte riflette il colore di una finestra
                // il grigio cambia tinta e il bianco no: e i due tornano a
                // sembrare materiali diversi proprio quando si nota di piu'.
                int mr = (sh * 4 + ur) / 5;
                int mg = (sh * 4 + ug) / 5;
                int mb = (sh * 4 + ub) / 5;
                if (mr > 255) mr = 255;
                if (mg > 255) mg = 255;
                if (mb > 255) mb = 255;

                uint32_t mask = 0xFF000000 | (mr << 16) | (mg << 8) | mb;
                blend_pixel(px, py, mask, cv * in / 255);
            }
        }
    }
    // Occhi: due cerchi pieni nella calotta superiore, senza pupille.
    // Si scambiano rosso e verde a ogni secondo.
    uint32_t eye_l = (rtc_second & 1) ? 0xFFF52A1C : 0xFF1C74F5;
    uint32_t eye_r = (rtc_second & 1) ? 0xFF1C74F5 : 0xFFF52A1C;
    int eye_y = by2 + 6;

    fill_circle_aa(sep_cx - 3, eye_y, 2, eye_l);
    fill_circle_aa(sep_cx + 3, eye_y, 2, eye_r);
    
}


// ----------------------------------------------------------------------------
// Animazione di minimizzazione (effetto lampada magica): sagoma a imbuto che
// si stringe verso il bottone della taskbar. Legge solo variabili globali.
// ----------------------------------------------------------------------------
static void render_genie_animation(void) {
    if (!anim_active || anim_h <= 0) return;

    uint32_t bg_color        = 0xFFFFFFFF; // Bianco puro di fondo
    uint32_t pinstripe_color = 0xFFE8E8E8; // Grigio Pinstripe
    uint32_t border_color    = 0xFF808080; // Bordo di definizione

    // 1. Calcolo del progresso temporale dell'animazione (da 0 a 100)
    int prog = 0;
    if (anim_frames_total > 0) {
        prog = ((anim_frames_total - anim_frames_left) * 100) / anim_frames_total;
    }
    if (prog > 100) prog = 100;

    // 2. Centri di gravità e larghezze globali
    int c_start   = anim_start_x + (anim_start_w / 2);
    int c_target  = anim_target_x + (anim_target_w / 2);
    int c_current = anim_x + (anim_w / 2);

    // 3. Progresso orizzontale accelerato per la base (si allinea nel primo 50% del tempo)
    int align_linear = prog * 2;
    if (align_linear > 100) align_linear = 100;

    // Applico una curva Ease-Out quadratica all'allineamento per una frenata morbida sul bottone
    int align_p = (align_linear * (200 - align_linear)) / 100;

    // Target dinamici ed esclusivi per la base della finestra:
    // A inizio animazione (align_p=0) la base è dritta e larga quanto l'app originale.
    // Scorrendo verso align_p=100, si sposta e si stringe per incastrarsi sul bottone.
    int c_bottom_target = c_start + ((c_target - c_start) * align_p) / 100;
    int w_bottom_target = anim_start_w + ((anim_target_w - anim_start_w) * align_p) / 100;

    // Disegniamo la sagoma a imbuto riga per riga
    for (int ly = anim_y; ly < anim_y + anim_h; ly++) {
        int dy = ly - anim_y; // Distanza dalla cima

        // A. MATEMATICA DELL'IMBUTO (Svasatura a Tromba)
        int t = (dy * 100) / anim_h;
        int inv_t = 100 - t;
        int pull = 100 - ((inv_t * inv_t * inv_t) / 10000);

        // B. Interpolazione geometrica della riga mirata ai target dinamici della base
        int cx = c_current + ((c_bottom_target - c_current) * pull) / 100;
        int lw = anim_w + ((w_bottom_target - anim_w) * pull) / 100;

        if (lw < 4) lw = 4; // Sicurezza di rendering
        int rx = cx - (lw / 2);

        // C. COLORAZIONE RIGA CON PINSTRIPE
        if (dy <= 1 || dy >= anim_h - 2) {
            draw_rect(rx, ly, lw, 1, border_color);
        } else {
            draw_rect(rx, ly, 2, 1, border_color);
            draw_rect(rx + lw - 2, ly, 2, 1, border_color);

            uint32_t row_color = (dy % 4 >= 2) ? pinstripe_color : bg_color;
            draw_rect(rx + 2, ly, lw - 4, 1, row_color);
        }
    }
}
// ----------------------------------------------------------------------------
// Nuvoletta sopra il vassoio di casa vuoto: dice quale applicazione sta usando
// la mascotte. Stesso stile di quella del percorso sulle finestre, ma rivolta
// verso il basso, perche' la taskbar e' in fondo allo schermo.
// ----------------------------------------------------------------------------
static void draw_mascot_tip(void) {
    if (!mascot_tip) return;
    if (mascot_host < 0 || mascot_host >= MAX_WINDOWS || !windows[mascot_host].active) {
        mascot_tip = 0;
        return;
    }

    // "Robot position: " seguito dal nome dell'applicazione. Composto a mano
    // perche' nel kernel non c'e' una strcat, e con un tetto alla lunghezza:
    // un titolo lunghissimo non deve sforare il buffer.
    char msg[96];
    const char* pre  = "Robot position: ";
    const char* name = windows[mascot_host].title;
    int k = 0;
    for (int i = 0; pre[i]  && k < 95; i++) msg[k++] = pre[i];
    for (int i = 0; name[i] && k < 95; i++) msg[k++] = name[i];
    msg[k] = '\0';

    int bw = get_prop_string_width(msg) + 20;
    int bh = 24;

    int tip_x = home_tray_cx;
    // La coda si posa sul bordo superiore del vetro della taskbar: la punta
    // cade sulla riga appena sopra, cosi' tocca il filo nero senza coprirlo.
    int point = taskbar_panel_top() - 1;
    int bx = tip_x - bw / 2;
    int by = point - bh - 4;
    if (bx < 2) bx = 2;
    if (bx + bw > fb_width - 2) bx = fb_width - 2 - bw;

    uint32_t bg = active_theme.tooltip_bg, ink = 0xFF000000;

    draw_rect(bx + 1, by, bw - 2, bh, bg);
    draw_rect(bx, by + 1, bw, bh - 2, bg);
    draw_rect(bx + 1, by, bw - 2, 1, ink);
    draw_rect(bx + 1, by + bh - 1, bw - 2, 1, ink);
    draw_rect(bx, by + 1, 1, bh - 2, ink);
    draw_rect(bx + bw - 1, by + 1, 1, bh - 2, ink);

    // Triangolo verso il basso: la prima riga riapre il bordo della nuvola
    for (int ty = 0; ty < 6; ty++) {
        int tw = 11 - ty * 2;
        int tx = tip_x - 5 + ty;
        int cy = by + bh - 1 + ty;
        draw_rect(tx, cy, tw, 1, bg);
        draw_pixel(tx - 1, cy, ink);
        draw_pixel(tx + tw, cy, ink);
    }

    draw_prop_string(msg, bx + 10, by + 4, ink);
}
// ----------------------------------------------------------------------------
// Nuvoletta informativa (tooltip stile XP) sopra l'icona della finestra attiva.
// Legge solo variabili globali.
// ----------------------------------------------------------------------------
static void render_tooltip(void) {
    if (active_tooltip_window == -1 || !windows[active_tooltip_window].active) return;

    window_t* twin = &windows[active_tooltip_window];

    int text_w = get_prop_string_width(twin->full_path);
    int bub_w = text_w + 20; // 10px di padding interno
    int bub_h = 24;

    int ix = twin->x + 6;
    int iy = twin->y + 8;

    // Punta del triangolo (Centro-basso dell'icona)
    int tip_x = ix + 8;
    int tip_y = iy - 2;

    int bx = tip_x - 12;

    // ABBASSATA DI 2 PIXEL: Ora il bordo inferiore combacia perfettamente col triangolo!
    int by = tip_y - 4 - bub_h;

    // TEMATIZZATO: Colore di sfondo della nuvoletta
    uint32_t bub_color = active_theme.tooltip_bg;
    uint32_t border_color = 0xFF000000; // Nero fine

    // Sistema Anti-Uscita dallo schermo
    int draw_triangle = 1;
    if (by < 0) {
        by = iy + 22; // Nuvola SOTTO l'icona
        tip_y = by - 5;
        draw_triangle = 2; // Triangolo rovesciato
    }
    if (bx < 0) bx = 0;
    if (bx + bub_w > fb_width) bx = fb_width - bub_w;

    // 1. Corpo principale (Arrotondato 1px)
    draw_rect(bx + 1, by, bub_w - 2, bub_h, bub_color);
    draw_rect(bx, by + 1, bub_w, bub_h - 2, bub_color);

    // 2. Bordi del corpo
    draw_rect(bx + 1, by, bub_w - 2, 1, border_color);
    draw_rect(bx + 1, by + bub_h - 1, bub_w - 2, 1, border_color);
    draw_rect(bx, by + 1, 1, bub_h - 2, border_color);
    draw_rect(bx + bub_w - 1, by + 1, 1, bub_h - 2, border_color);

    // 3. Triangolo di connessione
    if (draw_triangle == 1) { // Punta in basso
        for(int ty = 0; ty < 6; ty++) {
            int tw = 1 + (ty * 2);
            int tx = tip_x - ty;
            int current_y = tip_y - ty;
            draw_rect(tx, current_y, tw, 1, bub_color);
            draw_pixel(tx - 1, current_y, border_color);
            draw_pixel(tx + tw, current_y, border_color);
        }
        // Cancelliamo esattamente gli 11 pixel di bordo nero sopra al triangolo
        draw_rect(tip_x - 5, by + bub_h - 1, 11, 1, bub_color);
    } else if (draw_triangle == 2) { // Punta in alto
        for(int ty = 0; ty < 6; ty++) {
            int tw = 1 + (ty * 2);
            int tx = tip_x - ty;
            int current_y = tip_y + ty;
            draw_rect(tx, current_y, tw, 1, bub_color);
            draw_pixel(tx - 1, current_y, border_color);
            draw_pixel(tx + tw, current_y, border_color);
        }
        draw_rect(tip_x - 4, by, 9, 1, bub_color);
    }

    // 4. Testo del percorso
    draw_prop_string(twin->full_path, bx + 10, by + 4, border_color);
}


// ----------------------------------------------------------------------------
// Overlay della barra di progresso (copia file). Legge solo variabili globali.
// ----------------------------------------------------------------------------
static void render_progress_overlay(void) {
    if (!show_progress_bar || progress_total <= 0) return;

    int pb_w = 300; int pb_h = 70;
    int pb_x = (fb_width - pb_w) / 2; int pb_y = (fb_height - pb_h) / 2;

    draw_window(pb_x, pb_y, pb_w, pb_h, 1);
    draw_prop_string_bold("Copia file in corso...", pb_x + 10, pb_y + 8, 0xFFFFFFFF);

    int fill_w = (progress_current * (pb_w - 20)) / progress_total;
    if (fill_w > pb_w - 20) fill_w = pb_w - 20; // Cap grafico

    draw_textfield(pb_x + 10, pb_y + 35, pb_w - 20, 14, 0xFFE1E1E1);
    if (fill_w > 0) draw_rect(pb_x + 12, pb_y + 37, fill_w - 4, 10, 0xFF05339b);

    // Calcolo sicuro percentuale (anti-overflow 32bit)
    int pct = 0;
    if (progress_total > 100) pct = progress_current / (progress_total / 100);
    else pct = (progress_current * 100) / progress_total;
    if (pct > 100) pct = 100;

    char pct_str[32]; itoa(pct, pct_str);
    int p_idx = 0; while(pct_str[p_idx]) p_idx++;
    pct_str[p_idx++] = '%'; pct_str[p_idx] = '\0';
    draw_prop_string_bold(pct_str, pb_x + (pb_w/2) - 10, pb_y + 34, 0xFFFFFFFF);
}


// ----------------------------------------------------------------------------
// I tre comandi della finestra. Estratti in funzione perche' a finestra normale
// si disegnano insieme alla barra del titolo, mentre da massimizzata vanno
// tracciati DOPO il pannello dei menu, che altrimenti li coprirebbe.
// ----------------------------------------------------------------------------
static void draw_window_controls(window_t* win, int w_idx, int is_top,
                                 int mouse_x, int mouse_y, int is_clicking) {
    int btn_x, max_x, min_x, btn_y;
    window_btns(win, &btn_x, &max_x, &min_x, &btn_y);

    uint32_t symbol_color = 0xFF000000;
    if (is_top && mouse_x >= min_x && mouse_x <= btn_x + 23 &&
        mouse_y >= btn_y && mouse_y <= btn_y + 19) {
        symbol_color = 0xFFFFFFFF;
    }

    uint32_t glass_light = 0xFFFFFFFF;
    uint32_t glass_shadow = 0xFF575757;

    // --- CHIUDI ---
    uint32_t close_bg = is_top ? active_theme.window_close_btn : active_theme.window_bg;
    uint32_t cur_close_light = glass_light;
    if (is_clicking && pressed_window_id == w_idx && pressed_window_btn == 1) {
        close_bg = 0xFF000000
            | ((((close_bg >> 16) & 0xFF) * 75 / 100) << 16)
            | ((((close_bg >> 8) & 0xFF) * 75 / 100) << 8)
            |   ((close_bg & 0xFF) * 75 / 100);
        cur_close_light = 0xFFC0C0C0;
    }
    draw_button(btn_x, btn_y, 24, 20, close_bg, cur_close_light, glass_shadow);
    draw_ui_glyph_16(ui_glyph_close, btn_x + 4, btn_y + 2, symbol_color);

    // --- MASSIMIZZA ---
    uint32_t max_bg = active_theme.window_ctrl_btn;
    uint32_t cur_max_light = glass_light;
    if (is_clicking && pressed_window_id == w_idx && pressed_window_btn == 2) {
        max_bg = 0xFF000000
            | ((((max_bg >> 16) & 0xFF) * 75 / 100) << 16)
            | ((((max_bg >> 8) & 0xFF) * 75 / 100) << 8)
            |   ((max_bg & 0xFF) * 75 / 100);
        cur_max_light = 0xFFC0C0C0;
    }
    draw_button(max_x, btn_y, 24, 20, max_bg, cur_max_light, glass_shadow);
    if (win->is_maximized) draw_ui_glyph_16(ui_glyph_restore, max_x + 4, btn_y + 2, symbol_color);
    else                   draw_ui_glyph_16(ui_glyph_maximize, max_x + 4, btn_y + 2, symbol_color);

    // --- MINIMIZZA ---
    uint32_t min_bg = active_theme.window_ctrl_btn;
    uint32_t cur_min_light = glass_light;
    if (is_clicking && pressed_window_id == w_idx && pressed_window_btn == 3) {
        min_bg = 0xFF000000
            | ((((min_bg >> 16) & 0xFF) * 75 / 100) << 16)
            | ((((min_bg >> 8) & 0xFF) * 75 / 100) << 8)
            |   ((min_bg & 0xFF) * 75 / 100);
        cur_min_light = 0xFFC0C0C0;
    }
    draw_button(min_x, btn_y, 24, 20, min_bg, cur_min_light, glass_shadow);
    draw_ui_glyph_16(ui_glyph_minimize, min_x + 4, btn_y + 2, symbol_color);
}

// I tre comandi (chiudi, massimizza, minimizza) disegnati "dal vivo" sopra la
// finestra, a ogni fotogramma, invece di stare nella sua memoria. Cambiano
// aspetto al passaggio del mouse e alla pressione: tenendoli fuori, quei
// cambiamenti costano il disegno di tre pulsantini, non dell'intera finestra.
// La finestra massimizzata e il menu principale non hanno comandi qui: la
// prima li ha nel pannello, disegnati a parte.
static void draw_live_controls(window_t* win, int w, int is_top,
                               int mouse_x, int mouse_y, int is_clicking) {
    if (win->type == WIN_TYPE_START_MENU || win->is_maximized) return;
    draw_window_controls(win, w, is_top, mouse_x, mouse_y, is_clicking);
}

// ----------------------------------------------------------------------------
// MENU DELLA BARRA SUPERIORE
// Il pannello appartiene alla finestra che ha il fuoco: se quella espone dei
// menu li mostra, altrimenti resta al desktop e porta il nome del sistema.
// Geometria in un posto solo: disegno e click devono concordare al pixel.
// ----------------------------------------------------------------------------
#define MENU_PAD   10          // respiro ai lati di ogni titolo
#define MENU_ITEM_H 20         // altezza di una voce a tendina

static int menu_title_box(window_t* win, int idx, int* out_w) {
    int x = 8;
    for (int i = 0; i < idx; i++)
        x += get_prop_string_width_bold(win->menu_titles[i]) + MENU_PAD * 2;
    if (out_w) *out_w = get_prop_string_width_bold(win->menu_titles[idx]) + MENU_PAD * 2;
    return x;
}

static int menu_drop_w(window_t* win, int idx) {
    int w = 60;
    for (int i = 0; i < win->menu_item_count[idx]; i++) {
        int tw = get_prop_string_width_bold(win->menu_items[idx][i]) + 28;
        if (tw > w) w = tw;
    }
    return w;
}

// Indice della finestra che possiede la barra, oppure -1 (barra del desktop).
static int panel_owner(void) {
    int f = global_active_win;
    if (f >= 0 && f < MAX_WINDOWS && windows[f].active && !windows[f].is_minimized) return f;
    return -1;
}

static void draw_panel_menus(int mouse_x, int mouse_y) {
    int f = panel_owner();

    // Nessuna finestra selezionata: la barra e' del sistema
    if (f < 0 || windows[f].menu_count <= 0) {
        draw_prop_string_bold_shadow("III-Millennium OS", 12, 4, 2, 0xFF000000, 34);
        draw_prop_string_bold("III-Millennium OS", 12, 4, 0xFF000000);
        return;
    }

    window_t* win = &windows[f];
    for (int i = 0; i < win->menu_count; i++) {
        int bw, bx = menu_title_box(win, i, &bw);
        int open = (open_menu_win == f && open_menu_idx == i);
        int hot  = (mouse_x >= bx && mouse_x < bx + bw && mouse_y < PANEL_BAR_H);

        if (open || hot) {
            uint32_t hl = open ? 0xFF1E5FA8 : 0xFFFFFFFF;
            int a = open ? 255 : 90;
            for (int py = 1; py < PANEL_BAR_H - 1; py++)
                for (int px = bx; px < bx + bw; px++)
                    blend_pixel(px, py, hl, a);
        }

        uint32_t tc = open ? 0xFFFFFFFF : 0xFF000000;
        if (!open) draw_prop_string_bold_shadow(win->menu_titles[i], bx + MENU_PAD, 4, 2, 0xFF000000, 30);
        draw_prop_string_bold(win->menu_titles[i], bx + MENU_PAD, 4, tc);
    }
}

// La tendina esce dal pannello e copre le finestre: va disegnata per ultima.
static void draw_panel_dropdown(int mouse_x, int mouse_y) {
    if (open_menu_win < 0 || open_menu_idx < 0) return;
    int f = open_menu_win;
    if (f >= MAX_WINDOWS || !windows[f].active) { open_menu_win = -1; return; }

    window_t* win = &windows[f];
    if (open_menu_idx >= win->menu_count) { open_menu_idx = -1; return; }

    int n = win->menu_item_count[open_menu_idx];
    if (n <= 0) return;

    int dx = menu_title_box(win, open_menu_idx, 0);
    int dy = PANEL_BAR_H;
    int dw = menu_drop_w(win, open_menu_idx);
    int dh = n * MENU_ITEM_H + 6;

    draw_window_shadow(dx, dy, dw, dh);
    draw_rect(dx, dy, dw, dh, 0xFFF2F2F2);
    draw_rect(dx, dy, dw, 1, 0xFF000000);
    draw_rect(dx, dy + dh - 1, dw, 1, 0xFF000000);
    draw_rect(dx, dy, 1, dh, 0xFF000000);
    draw_rect(dx + dw - 1, dy, 1, dh, 0xFF000000);

    for (int i = 0; i < n; i++) {
        int iy = dy + 3 + i * MENU_ITEM_H;
        int hot = (mouse_x > dx && mouse_x < dx + dw - 1 &&
                   mouse_y >= iy && mouse_y < iy + MENU_ITEM_H);
        // Stesso azzurro chiaro del corpo dei bottoni, cosi' l'evidenziazione
        // parla la stessa lingua del resto dell'interfaccia.
        if (hot) draw_rect(dx + 1, iy, dw - 2, MENU_ITEM_H, active_theme.button_body);
        draw_prop_string_bold(win->menu_items[open_menu_idx][i], dx + 14, iy + 2, 0xFF000000);
    }
}
// Copertura antialiasata di un'ellisse. Serve una funzione dedicata perche'
// con due cerchi la mezzaluna viene per forza profonda: sono gli assi
// indipendenti a permettere una conca bassa e larga.
static int ellipse_cov(int px, int py, int cx, int cy, int rx, int ry) {
    if (rx < 1 || ry < 1) return 0;
    int rr = rx * ry * 8;
    int hits = 0;
    for (int sy = 0; sy < 4; sy++) {
        int fy = (py * 8 + sy * 2 + 1) - (cy * 8 + 4);
        for (int sx = 0; sx < 4; sx++) {
            int fx = (px * 8 + sx * 2 + 1) - (cx * 8 + 4);
            long a = (long)fx * ry, b = (long)fy * rx;
            if (a * a + b * b <= (long)rr * rr) hits++;
        }
    }
    return hits * 255 / 16;
}
// ----------------------------------------------------------------------------
// Vassoio a conca. Un'ellissi larga e bassa a cui se ne sottrae una seconda
// spostata in alto: ne esce una scodella con i bordi appena rialzati.
// La finitura e' la stessa della mascotte e della mensola: luce a sinistra,
// cuore scuro, seconda luce sul bordo destro. Sono i due picchi separati a
// farla leggere come metallo invece che come plastica grigia.
// 'base' e' la riga piu' bassa del vassoio.
// ----------------------------------------------------------------------------
static void draw_tray(int cx, int base, uint32_t fill, uint32_t rim) {
    (void)fill;
    const int RX = 8, RY = 4, IRX = 7, IRY = 3, DY = 1;
    int cy = base - RY;

    static const short tray_metal[17] = {
        150, 198, 236, 255, 242, 208, 168, 134, 112,
        118, 138, 170, 206, 238, 250, 216, 176
    };

    for (int py = cy - RY - 1; py <= cy + RY + 1; py++) {
        for (int px = cx - RX - 1; px <= cx + RX + 1; px++) {
            int o = ellipse_cov(px, py, cx, cy, RX, RY);
            if (o <= 0) continue;
            int i = ellipse_cov(px, py, cx, cy - DY, IRX, IRY);
            int cov = o - i;
            if (cov <= 0) continue;

            // Sagoma rientrata di un pixel su entrambi i bordi: e' lo spessore
            // del filo scuro, lo stesso di mensola e mascotte.
            int o2 = ellipse_cov(px, py, cx, cy, RX - 1, RY - 1);
            int i2 = ellipse_cov(px, py, cx, cy - DY, IRX + 1, IRY + 1);
            int inr = o2 - i2;
            if (inr < 0) inr = 0;

            int col = px - (cx - RX);
            if (col < 0) col = 0;
            if (col > 16) col = 16;

            int v = tray_metal[col];
            uint32_t body = 0xFF000000 | (v << 16) | (v << 8) | v;

            blend_pixel(px, py, body, cov * inr / 255);
            if (inr < 255) blend_pixel(px, py, rim, cov * (255 - inr) / 255);
        }
    }
}

// Hash deterministico. Le traiettorie devono dipendere solo dal tempo e
// dall'indice: se fossero casuali a ogni fotogramma le particelle
// sfarfallerebbero invece di salire.
static unsigned tray_hash(int a) {
    unsigned h = (unsigned)a * 2654435761u;
    h ^= h >> 15; h *= 2246822519u; h ^= h >> 13;
    return h;
}

// ----------------------------------------------------------------------------
// Particelle che salgono dalla conca del vassoio. Ognuna ha il proprio
// sfasamento, cosi' il flusso e' continuo invece che a ondate, e una deriva
// laterale che cresce con la quota, come una scintilla che si allarga salendo.
// ----------------------------------------------------------------------------
static void draw_tray_particles(int cx, int base, int seed) {
    const int N    = 8;
    const int LIFE = 130;                 // tick di un ciclo completo
    const int RISE = 20;                  // quanto salgono, in pixel
    // La barra del titolo e' chiara (luminanza 204): un alone chiaro non ha
    // margine per schiarire e sparisce. Qui l'alone e' viola PROFONDO, che
    // contro la sabbia stacca di oltre cento punti, e il nucleo resta chiaro.
    // Cosi' la particella si legge come una gemma accesa invece che come un
    // velo. E' la stessa famiglia di viola della mascotte, solo piu' saturo.
    const uint32_t VIOLA  = 0xFF7C3AED;   // alone profondo
    const uint32_t NUCLEO = 0xFFF3E8FF;   // cuore quasi bianco

    for (int i = 0; i < N; i++) {
        unsigned h = tray_hash(seed * 977 + i * 131);

        int t = (system_ticks + (int)(h % LIFE) + i * LIFE / N) % LIFE;
        int p = t * 255 / LIFE;           // avanzamento del ciclo, 0..255

        int y = base - 4 - p * RISE / 255;
        int x = cx + (((int)((h >> 8) & 7)) - 3) * p / 255;

        // Nasce in fretta, si spegne lentamente
        int a = (p < 60) ? (p * 255 / 60) : (255 - (p - 60) * 255 / 195);
        if (a <= 0) continue;
        a = a * (205 + (int)((h >> 16) & 50)) / 255;   // ogni scintilla ha la sua luce

        // Disco di circa cinque pixel: il colore passa dal nucleo chiaro al
        // viola della mascotte allontanandosi dal centro, e l'intensita' cala
        // con il quadrato della distanza. E' quel doppio gradiente, di tinta
        // e di luce insieme, a far sembrare la particella accesa invece che
        // semplicemente colorata.
        int nr = (NUCLEO >> 16) & 0xFF, ng = (NUCLEO >> 8) & 0xFF, nb = NUCLEO & 0xFF;
        int vr = (VIOLA  >> 16) & 0xFF, vg = (VIOLA  >> 8) & 0xFF, vb = VIOLA  & 0xFF;

        for (int oy = -3; oy <= 3; oy++) {
            for (int ox = -3; ox <= 3; ox++) {
                int d2 = ox * ox + oy * oy;
                if (d2 > 9) continue;

                int t = d2 * 255 / 9;              // 0 al centro, 255 al bordo
                int cr = nr + (vr - nr) * t / 255;
                int cg = ng + (vg - ng) * t / 255;
                int cb = nb + (vb - nb) * t / 255;

                int wgt = 255 - t;
                wgt = wgt * wgt / 255;             // caduta morbida

                blend_pixel(x + ox, y + oy,
                            0xFF000000 | (cr << 16) | (cg << 8) | cb,
                            a * wgt / 255);
            }
        }
    }
}

// ============================================================================
// CONTATORE DEI MILLISECONDI PER FOTOGRAMMA
// Il timer di sistema batte ogni 10 ms: per un fotogramma e' troppo grossolano.
// Si usa il contatore di cicli della CPU (rdtsc), tarato di continuo contro il
// timer: ogni secondo si misura quanti cicli sono trascorsi e se ne ricava
// quanti ne vale un millisecondo. Cosi' la misura resta valida anche se la
// frequenza della CPU (o dell'emulatore) cambia.
// ============================================================================
static inline uint64_t rdtsc64(void) {
    uint32_t lo, hi;
    __asm__ volatile ("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

static uint64_t perf_tsc_ref = 0;
static uint32_t perf_tick_ref = 0;
static uint32_t perf_cyc_per_ms = 0;   // 0 = taratura non ancora pronta
static uint32_t perf_frame_x10 = 0;    // durata media del fotogramma, in decimi di ms

static void perf_calibrate(void) {
    uint64_t now = rdtsc64();
    uint32_t t = system_ticks;
    if (perf_tsc_ref == 0) { perf_tsc_ref = now; perf_tick_ref = t; return; }
    uint32_t dt = t - perf_tick_ref;
    if (dt >= 100) {                               // un secondo di campione
        perf_cyc_per_ms = (uint32_t)((now - perf_tsc_ref) / (uint64_t)(dt * 10));
        perf_tsc_ref = now;
        perf_tick_ref = t;
    }
}

static void perf_record(uint64_t t0, uint64_t t1) {
    if (!perf_cyc_per_ms) return;
    uint32_t cur = (uint32_t)((t1 - t0) * 10 / perf_cyc_per_ms);
    // Media mobile: il valore istantaneo cambierebbe a ogni fotogramma e non
    // si riuscirebbe a leggerlo
    perf_frame_x10 = (perf_frame_x10 * 7 + cur) / 8;
}

// Scomposizione del fotogramma in fasi. Ogni perf_mark chiude una fase e le
// attribuisce il tempo trascorso dal segno precedente.
#define PERF_N 6
static const char* perf_label[PERF_N] = {
    "sfondo", "finestre", "barra", "taskbar", "extra", "swap"
};
static uint32_t perf_part_x10[PERF_N];
static uint64_t perf_mark_t;

static void perf_mark(int part) {
    uint64_t now = rdtsc64();
    if (perf_cyc_per_ms) {
        uint32_t cur = (uint32_t)((now - perf_mark_t) * 10 / perf_cyc_per_ms);
        perf_part_x10[part] = (perf_part_x10[part] * 7 + cur) / 8;
    }
    perf_mark_t = now;
}

// Scrive "12.3" in coda alla stringa, restituisce la nuova lunghezza
static int perf_put_x10(char* s, int k, uint32_t v) {
    uint32_t ip = v / 10;
    char d[12];
    int n = 0;
    do { d[n++] = (char)('0' + ip % 10); ip /= 10; } while (ip && n < 10);
    while (n) s[k++] = d[--n];
    s[k++] = '.';
    s[k++] = (char)('0' + v % 10);
    return k;
}

// Conservata per le prossime misure: unused evita l'avviso finche' resta spenta
static void __attribute__((unused)) draw_perf_counter(void) {
    char s[24];
    int k = 0;

    if (!perf_cyc_per_ms) {
        const char* w = "-- ms";
        while (w[k]) { s[k] = w[k]; k++; }
    } else {
        k = perf_put_x10(s, k, perf_frame_x10);
        s[k++] = ' '; s[k++] = 'm'; s[k++] = 's';
    }
    s[k] = '\0';

    // Totale allineato a destra, prima dei 120 pixel riservati a vassoio e
    // comandi della finestra massimizzata: non li copre mai.
    int tx = fb_width - 120 - get_prop_string_width(s);
    draw_prop_string(s, tx, 4, 0xFF404040);

    if (!perf_cyc_per_ms) return;

    // Scomposizione, in grigio piu' chiaro, subito a sinistra del totale
    char b[160];
    int j = 0;
    for (int i = 0; i < PERF_N; i++) {
        const char* l = perf_label[i];
        while (*l && j < 140) b[j++] = *l++;
        b[j++] = ' ';
        j = perf_put_x10(b, j, perf_part_x10[i]);
        if (i < PERF_N - 1) { b[j++] = ' '; b[j++] = ' '; }
    }
    b[j] = '\0';
    draw_prop_string(b, tx - 20 - get_prop_string_width(b), 4, 0xFF707070);
}

// ============================================================================
// MEMORIA DELLE FINESTRE
// Ogni finestra memorizza i propri pixel e li riusa finche' la sua "firma"
// non cambia. La firma e' un hash di tutto cio' che ne determina l'aspetto:
// se un fattore mancasse, la finestra resterebbe congelata su un'immagine
// vecchia, quindi qui dentro va messo TUTTO cio' che il suo disegno legge.
// Restano vivi a ogni fotogramma solo ombra, particelle e mascotte: la prima
// dipende da cio' che c'e' sotto, le altre due sono animate.
// ============================================================================
static uint32_t sig_u32(uint32_t h, uint32_t v) {
    h ^= v;
    h *= 16777619u;
    return h;
}

static uint32_t sig_bytes(uint32_t h, const void* p, int n) {
    const uint8_t* b = (const uint8_t*)p;
    for (int i = 0; i < n; i++) { h ^= b[i]; h *= 16777619u; }
    return h;
}

// Si puo' memorizzare? Solo finestre interamente a schermo e a dimensione
// normale, di un tipo stabile. Esclusi i menu, che reagiscono a ogni
// movimento, e le app con immagini: i loro pixel stanno nella memoria
// dell'app e possono cambiare senza che la firma se ne accorga.
static int window_cacheable(window_t* win) {
    // Anche la finestra massimizzata si memorizza: i suoi tre comandi, che
    // migrano nel pannello, si disegnano a parte dopo il pannello stesso.
    if (win->type != WIN_TYPE_TERMINAL && win->type != WIN_TYPE_APP_GUI) return 0;
    // Anche una finestra che sporge dal bordo si puo' memorizzare: basta che
    // possa stare tutta nello schermo, per poterla disegnare nella posizione di
    // studio quando il suo contenuto cambia (vedi studio_save).
    if (win->w > fb_width || win->h > fb_height) return 0;
    if (win->type == WIN_TYPE_APP_GUI)
        for (int i = 0; i < win->ui_count; i++)
            if ((win->ui_elements[i].type & 0xFF) == 5) return 0;
    return 1;
}

static uint32_t window_signature(int w, int is_top, int mouse_x, int mouse_y, int is_clicking) {
    window_t* win = &windows[w];
    uint32_t h = 2166136261u;

    // Cornice, barra del titolo, targhetta
    h = sig_u32(h, (uint32_t)win->w);
    h = sig_u32(h, (uint32_t)win->h);
    h = sig_u32(h, (uint32_t)win->type);
    h = sig_u32(h, (uint32_t)win->is_maximized);
    h = sig_u32(h, (uint32_t)is_top);
    h = sig_u32(h, (uint32_t)win->icon_id);
    h = sig_bytes(h, win->title, sizeof(win->title));
    h = sig_bytes(h, &active_theme, sizeof(active_theme));

    // I tre comandi della barra del titolo NON fanno parte della memoria: si
    // disegnano sopra la copia a ogni fotogramma (vedi draw_live_controls).
    // Se ci fossero, il loro cambiare aspetto al passaggio del mouse
    // costringerebbe a ridisegnare da zero l'intera finestra.

    if (win->type == WIN_TYPE_TERMINAL) {
        h = sig_bytes(h, win->t_history, sizeof(win->t_history));
        h = sig_bytes(h, win->t_buffer,  sizeof(win->t_buffer));
        h = sig_u32(h, (uint32_t)win->t_head);
        h = sig_u32(h, (uint32_t)win->t_len);
        h = sig_u32(h, (uint32_t)win->t_cursor);
        h = sig_u32(h, (uint32_t)win->t_scroll);
        h = sig_u32(h, (uint32_t)(is_top && blink_counter < 50));

        // Un'app in esecuzione nasconde la riga di comando
        int running = 0;
        for (int p = 1; p < MAX_PROCESSES; p++)
            if (process_table[p].active &&
                (process_table[p].linked_window == w || process_table[p].parent_window == w)) {
                running = 1;
                break;
            }
        h = sig_u32(h, (uint32_t)running);
    } else {
        h = sig_u32(h, (uint32_t)win->ui_count);
        // Aree di testo: il loro testo vive nel kernel, fuori dagli elementi;
        // il contatore di versione cambia a ogni modifica, cursore compreso.
        for (int t = 0; t < TA_MAX; t++) h = sig_u32(h, win->ta[t].version);
        h = sig_bytes(h, win->ui_elements, win->ui_count * (int)sizeof(win->ui_elements[0]));

        int focus = (focused_window_id == w);
        h = sig_u32(h, focus ? (uint32_t)focused_element_idx + 1 : 0);
        // Posizione del cursore nella textbox: le frecce la spostano senza
        // toccare il testo, quindi va nella firma a parte, altrimenti la
        // finestra resterebbe con il cursore fermo dov'era.
        h = sig_u32(h, focus ? (uint32_t)focused_cursor_pos : 0);
        h = sig_u32(h, (uint32_t)(focus && blink_counter < 50));

        // Solo la finestra in primo piano mostra i pulsanti premuti: per le
        // altre il mouse e' irrilevante, anche se ci passa sopra durante un
        // trascinamento. Includerlo le invaliderebbe a ogni fotogramma.
        if (is_top && is_clicking && mouse_x >= win->x && mouse_x < win->x + win->w &&
            mouse_y >= win->y && mouse_y < win->y + win->h) {
            h = sig_u32(h, (uint32_t)(mouse_x - win->x));
            h = sig_u32(h, (uint32_t)(mouse_y - win->y));
        }
    }
    return h;
}


// ============================================================================
// POSIZIONE DI STUDIO
// Quando il contenuto di una finestra che sporge dal bordo cambia, la parte
// fuori schermo non esiste nel backbuffer e non si potrebbe memorizzarla.
// Allora la si disegna per un istante in una "posizione di studio", tutta
// dentro lo schermo: si salva prima cio' che c'e' in quel punto, si disegna,
// si memorizza, e si rimette tutto com'era. Poi la memoria viene copiata al
// posto vero, tagliata dal bordo. Il margine SH_REACH tiene dentro anche
// l'ombra, che viene disegnata insieme alla finestra.
// ============================================================================
static uint32_t* studio_buf = 0;
static int studio_cap = 0;
static int studio_x, studio_y, studio_w, studio_h;

static int window_fully_visible(window_t* win) {
    return win->x >= 0 && win->y >= 0 &&
           win->x + win->w <= fb_width && win->y + win->h <= fb_height;
}

static int studio_save(int x, int y, int w, int h) {
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > fb_width)  w = fb_width - x;
    if (y + h > fb_height) h = fb_height - y;
    if (w <= 0 || h <= 0) return 0;

    if (w * h > studio_cap) {
        if (studio_buf) kfree(studio_buf);
        studio_buf = (uint32_t*)kmalloc((size_t)w * h * 4);
        studio_cap = studio_buf ? w * h : 0;
        if (!studio_buf) return 0;
    }
    for (int r = 0; r < h; r++) {
        uint32_t* src = &backbuffer[(y + r) * fb_width + x];
        uint32_t* dst = &studio_buf[r * w];
        for (int c = 0; c < w; c++) dst[c] = src[c];
    }
    studio_x = x; studio_y = y; studio_w = w; studio_h = h;
    return 1;
}

static void studio_restore(void) {
    for (int r = 0; r < studio_h; r++) {
        uint32_t* src = &studio_buf[r * studio_w];
        uint32_t* dst = &backbuffer[(studio_y + r) * fb_width + studio_x];
        for (int c = 0; c < studio_w; c++) dst[c] = src[c];
    }
}

// Copia la finestra appena disegnata dal backbuffer alla sua memoria
static void cache_capture(window_t* win) {
    if (!win->cache || win->cache_w != win->w || win->cache_h != win->h) {
        if (win->cache) kfree(win->cache);
        win->cache = (uint32_t*)kmalloc((size_t)win->w * win->h * 4);
        win->cache_w = win->w;
        win->cache_h = win->h;
        win->cache_ok = 0;
        if (!win->cache) return;     // memoria esaurita: si continua a disegnare dal vivo
    }
    // Si copiano solo le righe e le colonne che stanno nello schermo. Serve
    // alla finestra massimizzata, la cui barra del titolo sale oltre il bordo
    // alto: quelle righe restano sotto il pannello e non si vedono mai.
    for (int r = 0; r < win->h; r++) {
        int sy = win->y + r;
        if (sy < 0 || sy >= fb_height) continue;
        int c0 = 0, c1 = win->w;
        if (win->x + c0 < 0)        c0 = -win->x;
        if (win->x + c1 > fb_width) c1 = fb_width - win->x;
        uint32_t* src = &backbuffer[sy * fb_width + win->x];
        uint32_t* dst = &win->cache[r * win->w];
        for (int c = c0; c < c1; c++) dst[c] = src[c];
    }
    win->cache_ok = 1;
}


// Pixel che la cornice lascia scoperti in alto, riga per riga (vedi i bordi
// in draw_window). Sono trasparenti: nella memoria contengono lo sfondo del
// momento della cattura, che spostando la finestra non e' piu' quello giusto.
// Saltandoli si lascia vedere lo sfondo di questo fotogramma, gia' disegnato.
// Se un giorno cambi la forma degli angoli in draw_window, aggiorna qui.
static const int win_corner_inset[5] = { 5, 3, 2, 1, 1 };

static void cache_blit(window_t* win) {
    for (int r = 0; r < win->h; r++) {
        int sy = win->y + r;
        if (sy < 0 || sy >= fb_height) continue;          // riga fuori schermo

        int c0 = 0, c1 = win->w;
        if (r < 5) { c0 = win_corner_inset[r]; c1 = win->w - win_corner_inset[r]; }
        // Taglio ai bordi sinistro e destro dello schermo
        if (win->x + c0 < 0)        c0 = -win->x;
        if (win->x + c1 > fb_width) c1 = fb_width - win->x;

        uint32_t* src = &win->cache[r * win->w];
        uint32_t* dst = &backbuffer[sy * fb_width + win->x];
        for (int c = c0; c < c1; c++) dst[c] = src[c];
    }
}

// Cio' che resta vivo sopra la finestra: particelle e mascotte del vassoio
static void draw_window_overlays(window_t* win, int w) {
    if (win->is_maximized || !window_tray_type(w)) return;
    draw_tray_particles(win->x + 14, win->y + 27, w + 1);
    if (mascot_host == w && !mascot_drag)
        draw_mascot(mascot_seat_x(win), mascot_seat_y(win));
}

// ============================================================================
// RIDIMENSIONAMENTO A CORNICE
// Mentre si trascina l'angolo, la finestra resta com'e' (e quindi resta nella
// sua memoria, costando quanto una copia) e sopra si disegna solo un filo che
// mostra la misura nuova. Al rilascio la finestra cambia misura e si ridisegna
// una volta sola. E' il modo in cui lo facevano Mac OS 9 e Windows 95.
// ============================================================================
static void frame_rect(int x, int y, int w, int h, uint32_t c, int a) {
    if (w <= 0 || h <= 0) return;
    for (int i = 0; i < w; i++) { blend_pixel(x + i, y, c, a); blend_pixel(x + i, y + h - 1, c, a); }
    for (int j = 1; j < h - 1; j++) { blend_pixel(x, y + j, c, a); blend_pixel(x + w - 1, y + j, c, a); }
}

static int put_uint(char* s, int k, int v) {
    char d[12];
    int n = 0;
    do { d[n++] = (char)('0' + v % 10); v /= 10; } while (v && n < 11);
    while (n) s[k++] = d[--n];
    return k;
}

static void draw_resize_frame(void) {
    if (resizing_window < 0 || !windows[resizing_window].active) return;
    int x = windows[resizing_window].x, y = windows[resizing_window].y;
    int w = resize_frame_w, h = resize_frame_h;

    // Doppio filo, scuro fuori e chiaro dentro: si vede su qualunque sfondo
    frame_rect(x, y, w, h, 0xFF000000, 150);
    frame_rect(x + 1, y + 1, w - 2, h - 2, 0xFFFFFFFF, 170);

    // La misura, in una piccola etichetta scura nell'angolo in basso a destra
    char s[24];
    int k = put_uint(s, 0, w);
    s[k++] = ' '; s[k++] = 'x'; s[k++] = ' ';
    k = put_uint(s, k, h);
    s[k] = '\0';

    int tw = get_prop_string_width_bold(s);
    int pw = tw + 14, ph = 20;
    int px0 = x + w - pw - 10, py0 = y + h - ph - 10;
    if (px0 < x + 6) px0 = x + 6;
    if (py0 < y + 6) py0 = y + 6;

    for (int py = py0; py < py0 + ph; py++)
        for (int px = px0; px < px0 + pw; px++) {
            int cov = rrect_coverage(px, py, px0, py0, pw, ph, 6);
            if (cov > 0) blend_pixel(px, py, 0xFF000000, cov * 150 / 255);
        }
    draw_prop_string_bold(s, px0 + 7, py0 + 2, 0xFFFFFFFF);
}


void render_desktop(int mouse_x, int mouse_y, int is_clicking) {
    // Mentre si ridimensiona, il tasto premuto serve alla cornice e non ai
    // pulsanti. Considerarlo farebbe cambiare la firma della finestra a ogni
    // movimento del cursore al suo interno, costringendola a ridisegnarsi di
    // continuo proprio mentre si stringe la cornice.
    if (resizing_window != -1) is_clicking = 0;
    uint64_t perf_t0 = rdtsc64();
    perf_calibrate();
    perf_mark_t = perf_t0;
    // =======================================================
    // SFONDO DESKTOP (Tematizzato)
    // =======================================================
    draw_rect(0, 0, fb_width, fb_height, active_theme.desktop_bg);
    perf_mark(0);   // sfondo

    // --- CERCHIAMO LA FINESTRA IN PRIMO PIANO ---
    
    int top_app_win = global_active_win;

    // Disegniamo le finestre dal fondo alla cima
    for(int order_idx = MAX_WINDOWS -1; order_idx >= 0; order_idx--) {
        int w = window_order[order_idx]; 
        if (windows[w].active) {
            window_t* win = &windows[w];
            
            // Salta il disegno se la finestra è minimizzata O ancora in inizializzazione
            if (win->is_minimized || win->is_initializing) continue;

            // MEMORIA DELLA FINESTRA: se nulla e' cambiato, si copia e basta.
            // Durante il ridimensionamento la misura cambia a ogni fotogramma:
            // memorizzare costringerebbe a riallocare di continuo, meglio di no.
            int use_cache = window_cacheable(win);
            uint32_t wsig = 0;
            if (use_cache) {
                wsig = window_signature(w, w == top_app_win, mouse_x, mouse_y, is_clicking);
                if (win->cache_ok && win->cache && win->cache_w == win->w &&
                    win->cache_h == win->h && win->cache_sig == wsig) {
                    // Stessa regola del ridisegno: la massimizzata non ha ombra
                    if (!win->is_maximized)
                        draw_window_shadow(win->x, win->y, win->w, win->h);
                    cache_blit(win);
                    draw_live_controls(win, w, w == top_app_win, mouse_x, mouse_y, is_clicking);
                    draw_window_overlays(win, w);
                    continue;
                }
            }

            // La finestra sporge e il suo contenuto e' cambiato: la si disegna
            // nella posizione di studio. Il mouse si sposta insieme a lei, cosi'
            // pulsanti premuti e passaggi del puntatore restano al loro posto.
            int studio = 0;
            int real_x = win->x, real_y = win->y;
            int real_mx = mouse_x, real_my = mouse_y;
            // La finestra massimizzata non va in studio: sporge solo dal bordo
            // alto, sotto il pannello, e quelle righe non si vedono mai.
            if (use_cache && !window_fully_visible(win) && !win->is_maximized) {
                int tx = win->x, ty = win->y;
                if (tx < 0) tx = 0;
                if (ty < 0) ty = 0;
                if (tx + win->w > fb_width)  tx = fb_width - win->w;
                if (ty + win->h > fb_height) ty = fb_height - win->h;

                if (studio_save(tx - SH_REACH, ty, win->w + 2 * SH_REACH, win->h + SH_REACH)) {
                    studio = 1;
                    mouse_x += tx - real_x;
                    mouse_y += ty - real_y;
                    win->x = tx;
                    win->y = ty;
                } else {
                    use_cache = 0;    // memoria esaurita: si disegna dal vivo, come prima
                }
            }

            if (win->type != WIN_TYPE_START_MENU) {
                
                // Controlliamo se questa specifica finestra è quella in cima
                int is_top = (w == top_app_win); 
                
                // Questo ci serve per il drop shadow delle finestre
                if (!win->is_maximized)
                    draw_window_shadow(win->x, win->y, win->w, win->h);

                // Passiamo l'informazione alla nostra funzione aggiornata
                draw_window(win->x, win->y, win->w, win->h, is_top);
                


                // Icona, badge e titolo esistono solo a finestra normale: da
                // massimizzata la barra del titolo scorre sotto il pannello.
                if (!win->is_maximized) {
                    // Il terminale tiene la sua icona; tutte le altre finestre
                    // hanno il vassoio, appoggiato sulla stessa riga di base.
                    if (win->type == WIN_TYPE_TERMINAL)
                        draw_icon_16(win->icon_id, win->x + 6, win->y + 8);
                    else
                    {
                        // Solo il vassoio, che e' statico e finisce in memoria.
                        // Particelle e mascotte si disegnano dopo, sopra la copia.
                        draw_tray(win->x + 14, win->y + 27, 0xFFC8C8C8, 0xFF6E6E6E);
                    }

                    int text_width_px = get_prop_string_width_bold(win->title);
                    uint32_t dec_color = active_theme.titlebar_base;
                    uint32_t border_color = 0xFFFFFFFF;

                    int badge_x = win->x + 27;   // due pixel di respiro per il vassoio
                    int badge_y = win->y + 5;
                    int badge_w = text_width_px + 8;
                    int badge_h = 22;

                    draw_rect(badge_x + 2, badge_y, badge_w - 4, badge_h, border_color);
                    draw_rect(badge_x, badge_y + 2, badge_w, badge_h - 4, border_color);
                    draw_rect(badge_x + 2, badge_y + 1, badge_w - 4, badge_h - 2, dec_color);
                    draw_rect(badge_x + 1, badge_y + 2, badge_w - 2, badge_h - 4, dec_color);
                    draw_pixel(badge_x + 1, badge_y + 1, border_color);
                    draw_pixel(badge_x + badge_w - 2, badge_y + 1, border_color);
                    draw_pixel(badge_x + 1, badge_y + badge_h - 2, border_color);
                    draw_pixel(badge_x + badge_w - 2, badge_y + badge_h - 2, border_color);

                    draw_prop_string_bold(win->title, badge_x + 4, win->y + 8, 0xFF000000);

                    // I tre comandi si disegnano dopo, sopra la copia memorizzata:
                    // vedi draw_live_controls, alla fine di questo giro.
                }

            


                // --- ANGOLO DI RIDIMENSIONAMENTO ---
                
                int res_x = win->x + win->w - 16;
                int res_y = win->y + win->h - 16;
                
                uint32_t resize_color = is_top ? 0xFF808080 : 0xFF808080;
                
                // 1. Linee Esterne Nere (Accorciate da 16/14 a 12px per non tagliare la curva della finestra)
                draw_rect(res_x + 14, res_y, 2, 12, 0xFF000000); // Destra
                draw_rect(res_x, res_y + 14, 12, 2, 0xFF000000); // Basso
                
                // 2. Linee Interne Colorate (Accorciate in proporzione per incastrarsi)
                draw_rect(res_x + 12, res_y, 2, 10, resize_color); // Destra
                draw_rect(res_x, res_y + 12, 10, 2, resize_color); // Basso
            }
            // ----------------------------------------
            
            if (win->type == WIN_TYPE_TERMINAL) {
                int term_x = win->x + 4;
                int term_y = win->y + 32; // 4px (bordo) + 24px (barra) + 4px (spazio)
                int term_w = win->w - 8;  // 4px a sinistra e 4px a destra
                int term_h = win->h - 36; // 32px in alto e 4px in basso
                draw_rect(term_x, term_y, term_w, term_h, 0xFF000000);
                
                
                
                // Calcoliamo quante righe visibili ci stanno magicamente in base all'altezza
                // Abbiamo aumentato il margine inferiore (da 40 a 60) per ALZARE tutta l'area di stampa
                int calc_vis_rows = (term_h - 60) / 14; 
                if (calc_vis_rows > TERM_MAX_ROWS) calc_vis_rows = TERM_MAX_ROWS;
                
                
                // ==========================================
                // MOTORE DI IMPAGINAZIONE E SCROLL ASSOLUTO
                // ==========================================
                int max_chars_per_line = (term_w - 40) / 8;
                if (max_chars_per_line < 10) max_chars_per_line = 10;
                int max_visible_rows = (term_h - 60) / 14;

                // 1. PRE-CALCOLO: Quante righe visive occupa ogni comando in memoria?
                int row_vlines[TERM_MAX_ROWS];
                int total_vlines = 0;
                for (int r = 0; r < TERM_MAX_ROWS; r++) {
                    int real_r = (win->t_head + r) % TERM_MAX_ROWS; // LETTURA CIRCOLARE
                    
                    if (win->t_history[real_r][0] == '\0') {
                        row_vlines[real_r] = 0;
                    } else {
                        int lines = 0;
                        char* str = win->t_history[real_r];
                        int len = 0; while(str[len]) len++;
                        int i = 0;
                        while (i < len) {
                            if (len - i <= max_chars_per_line) {
                                lines++; break;
                            } else {
                                int bp = max_chars_per_line;
                                while(bp > 0 && str[i+bp] != ' ') bp--;
                                if (bp == 0) bp = max_chars_per_line;
                                lines++; i += bp; if (str[i] == ' ') i++;
                            }
                        }
                        row_vlines[real_r] = lines;
                        total_vlines += lines;
                    }
                }

                // 2. LOGICA SCROLLBAR PROPORZIONALE (Ora calcolata sulle righe VISIVE)
                int max_scrl = total_vlines - max_visible_rows;
                if (max_scrl < 0) max_scrl = 0;

                // Auto-correzione
                if (win->t_scroll > max_scrl) win->t_scroll = max_scrl;
                if (win->t_scroll < 0) win->t_scroll = 0;

                // Calcoliamo a quale "riga visiva assoluta" corrisponde la cima della finestra
                int top_visible_line = 0;
                if (total_vlines > max_visible_rows) {
                    top_visible_line = total_vlines - max_visible_rows - win->t_scroll;
                }

                // 3. DISEGNO TESTO
                int current_y = term_y + 10;
                int drawn_rows = 0;
                int current_vline = 0;

                for (int r = 0; r < TERM_MAX_ROWS && drawn_rows < max_visible_rows; r++) {
                    int real_r = (win->t_head + r) % TERM_MAX_ROWS; // LETTURA CIRCOLARE
                    if (row_vlines[real_r] == 0) continue;

                    char* str = win->t_history[real_r];
                    int len = 0; while(str[len]) len++;
                    int i = 0;

                    while (i < len && drawn_rows < max_visible_rows) {
                        int bp = len - i;
                        if (bp > max_chars_per_line) {
                            bp = max_chars_per_line;
                            while(bp > 0 && str[i+bp] != ' ') bp--;
                            if (bp == 0) bp = max_chars_per_line;
                        }

                        // Disegniamo solo se siamo scesi abbastanza da entrare nella "finestra visiva"
                        if (current_vline >= top_visible_line) {
                            char temp[150];
                            for(int t=0; t<bp; t++) temp[t] = str[i+t];
                            temp[bp] = '\0';
                            
                            draw_string(temp, term_x + 10, current_y, 0xFF00FF00, 1);
                            current_y += 14;
                            drawn_rows++;
                        }

                        current_vline++;
                        i += bp;
                        if (str[i] == ' ') i++;
                    }
                }

                // 4. DISEGNO GRAFICA SCROLLBAR (Container Scavato Minimalista)
                int sb_x = term_x + term_w - 20;
                int sb_h = term_h;

                // A. IL CANALE (Well) - Sfondo scavato tematizzato
                draw_textfield(sb_x, term_y, 20, sb_h, active_theme.scrollbar_track);

                // B. GLI INDICATORI DI SCORRIMENTO (Allineamento ottico corretto +1px a destra)
                // Indicatore SU 
                draw_rect(sb_x + 5, term_y + 9, 12, 2, 0xFF000000); 

                // Indicatore GIU 
                draw_rect(sb_x + 5, term_y + sb_h - 11, 12, 2, 0xFF000000);
                


                if (max_scrl > 0) {
                    int track_h = sb_h - 40; 
                    
                    int thmb_h = (max_visible_rows * track_h) / total_vlines;
                    if (thmb_h < 15) thmb_h = 15;
                    if (thmb_h > track_h) thmb_h = track_h;

                    int draggable_space = track_h - thmb_h;
                    int y_offset = ((max_scrl - win->t_scroll) * draggable_space) / max_scrl;
                    
                    int thmb_y = term_y + 20 + y_offset;
                    
                    // =======================================================
                    // SCROLLBAR THUMB (Stile Vetro Verticale Aqua Arrotondato)
                    // =======================================================
                    int t_x = sb_x + 2;
                    int t_y = thmb_y;
                    int t_w = 18; 
                    int t_h = thmb_h;
                    int half_w = t_w / 2;

                    uint32_t thumb_color = active_theme.primary_color;   // Colore Base
                    uint32_t line_color  = active_theme.secondary_color; // Colore Linee Scanalate
                    uint32_t shadow = 0xFF575757;
                    uint32_t border_out = 0xFF808080;

                    uint32_t br = (thumb_color >> 16) & 0xFF;
                    uint32_t bg = (thumb_color >> 8) & 0xFF;
                    uint32_t bb = thumb_color & 0xFF;

                    // A. LENTE BASE VERTICALE
                    for (int col = 1; col < t_w - 1; col++) {
                        uint32_t r, g, b;
                        if (col <= half_w) {
                            uint32_t darken = 15 - ((col * 15) / half_w); 
                            r = br - ((br * darken) / 100);
                            g = bg - ((bg * darken) / 100);
                            b = bb - ((bb * darken) / 100);
                        } else {
                            uint32_t lighten = (((col - half_w) * 45) / (t_w - half_w)); 
                            r = br + ((255 - br) * lighten / 100);
                            g = bg + ((255 - bg) * lighten / 100);
                            b = bb + ((255 - bb) * lighten / 100);
                        }
                        draw_rect(t_x + col, t_y + 1, 1, t_h - 2, 0xFF000000 | (r << 16) | (g << 8) | b);
                    }

                    // B. GRIP LINES (3 scanalature orizzontali al centro)
                    int center_y = t_y + (t_h / 2);
                    if (t_h > 15) { // Le disegniamo solo se il cursore è abbastanza alto
                        draw_rect(t_x + 3, center_y - 3, t_w - 6, 1, line_color);
                        draw_rect(t_x + 3, center_y,     t_w - 6, 1, line_color);
                        draw_rect(t_x + 3, center_y + 3, t_w - 6, 1, line_color);
                    }

                    // C. AQUA GLARE VERTICALE (Riflesso a sinistra)
                    for (int col = 2; col <= half_w; col++) {
                        uint32_t white_mix = 75 - ((col * 50) / half_w); 
                        for (int row = 1; row < t_h - 1; row++) {
                            // Leggiamo cosa c'è "sotto" (Linee + Fondo) e applichiamo la luce
                            uint32_t cp = backbuffer[(t_y + row) * fb_width + (t_x + col)];
                            uint32_t cr = (cp >> 16) & 0xFF;
                            uint32_t cg = (cp >> 8) & 0xFF;
                            uint32_t cb = cp & 0xFF;
                            
                            uint32_t gr = cr + ((255 - cr) * white_mix / 100);
                            uint32_t gg = cg + ((255 - cg) * white_mix / 100);
                            uint32_t gb = cb + ((255 - cb) * white_mix / 100);
                            draw_pixel(t_x + col, t_y + row, 0xFF000000 | (gr << 16) | (gg << 8) | gb);
                        }
                    }

                    // D. BORDI E RIFLESSI INTERNI
                    uint32_t il_r = br + ((255 - br) * 60 / 100);
                    uint32_t il_g = bg + ((255 - bg) * 60 / 100);
                    uint32_t il_b = bb + ((255 - bb) * 60 / 100);
                    uint32_t inner_light = 0xFF000000 | (il_r << 16) | (il_g << 8) | il_b;

                    draw_rect(t_x + 4, t_y, t_w - 8, 1, border_out);              
                    draw_rect(t_x, t_y + 4, 1, t_h - 8, border_out);             
                    draw_rect(t_x + 4, t_y + t_h - 1, t_w - 8, 1, shadow); 
                    draw_rect(t_x + t_w - 1, t_y + 4, 1, t_h - 8, shadow); 

                    draw_rect(t_x + 2, t_y + 1, t_w - 4, 1, inner_light);         
                    draw_rect(t_x + 1, t_y + 2, 1, t_h - 4, inner_light);        
                    draw_rect(t_x + 2, t_y + t_h - 2, t_w - 4, 1, shadow); 
                    draw_rect(t_x + t_w - 2, t_y + 2, 1, t_h - 4, shadow); 

                    // E. CURVATURE DEGLI ANGOLI (Anti-aliasing Aqua)
                    
                    // Top-Left (Luce incidente)
                    draw_pixel(t_x + 2, t_y + 1, border_out); draw_pixel(t_x + 3, t_y + 1, border_out);
                    draw_pixel(t_x + 1, t_y + 2, border_out); draw_pixel(t_x + 1, t_y + 3, border_out);
                    draw_pixel(t_x + 2, t_y + 2, inner_light); 
                    
                    // Top-Right (Raccordo nell'ombra, usa 'shadow' per ammorbidire!)
                    draw_pixel(t_x + t_w - 3, t_y + 1, border_out); draw_pixel(t_x + t_w - 4, t_y + 1, border_out);
                    draw_pixel(t_x + t_w - 2, t_y + 2, shadow);     draw_pixel(t_x + t_w - 2, t_y + 3, shadow);
                    draw_pixel(t_x + t_w - 3, t_y + 2, shadow);

                    // Bottom-Left (Chiude la luce a sinistra)
                    draw_pixel(t_x + 1, t_y + t_h - 3, border_out); draw_pixel(t_x + 1, t_y + t_h - 4, border_out);
                    draw_pixel(t_x + 2, t_y + t_h - 2, shadow);     draw_pixel(t_x + 3, t_y + t_h - 2, shadow);
                    draw_pixel(t_x + 2, t_y + t_h - 3, shadow); 

                    // Bottom-Right (Chiude l'ombra a destra)
                    draw_pixel(t_x + t_w - 2, t_y + t_h - 3, shadow); draw_pixel(t_x + t_w - 2, t_y + t_h - 4, shadow);
                    draw_pixel(t_x + t_w - 3, t_y + t_h - 2, shadow); draw_pixel(t_x + t_w - 4, t_y + t_h - 2, shadow);
                    draw_pixel(t_x + t_w - 3, t_y + t_h - 3, shadow);
                    // =======================================================
                    
                    // SALVIAMO I DATI PER IL TRASCINAMENTO
                    win->t_max_scrl = max_scrl;
                    win->t_thumb_y = thmb_y;
                    win->t_thumb_h = thmb_h;
                } else {
                    win->t_max_scrl = 0; // Scroll disattivato
                }
                


                // =======================================================
                // VERIFICA SE UN'APP STA GIRANDO IN QUESTO TERMINALE
                // =======================================================
                int app_running_here = 0;
                for(int p=1; p<MAX_PROCESSES; p++) {
                    if (process_table[p].active && (process_table[p].linked_window == w || process_table[p].parent_window == w)) {
                        app_running_here = 1; break;
                    }
                }

                // Disegniamo la riga di comando SOLO se il terminale è libero
                if (!app_running_here) {
                    draw_string("OS> ", term_x + 10, term_y + term_h - 30, 0xFFFFFFFF, 1);
                    
                    // --- SCORRIMENTO ORIZZONTALE DINAMICO DELL'INPUT ---
                    int max_input_chars = (term_w - 60) / 8;
                    int start_idx = 0;
                    if (win->t_cursor >= max_input_chars) start_idx = win->t_cursor - max_input_chars + 1;
                    
                    char temp_cmd[256]; 
                    int cmd_idx = 0;
                    for(int i = start_idx; i < win->t_len && cmd_idx < max_input_chars; i++) {
                        temp_cmd[cmd_idx++] = win->t_buffer[i];
                    }
                    temp_cmd[cmd_idx] = '\0';
                    
                    draw_string(temp_cmd, term_x + 42, term_y + term_h - 30, 0xFFFFFFFF, 1);

                    // --- CURSORE A BLOCCO INTERATTIVO ---
                    int is_top = (w == top_app_win); 
                    if (is_top && blink_counter < 50) {
                        int cursor_pos_visibile = win->t_cursor - start_idx;
                        if (cursor_pos_visibile >= 0 && cursor_pos_visibile <= max_input_chars) {
                            int cursor_x = term_x + 42 + (cursor_pos_visibile * 8);
                            int cursor_y = term_y + term_h - 30;
                            
                            draw_rect(cursor_x, cursor_y - 1, 8, 10, 0xFFFFFFFF);
                            if (win->t_cursor < win->t_len) {
                                char char_under = win->t_buffer[win->t_cursor];
                                char temp_str[2] = {char_under, '\0'};
                                draw_string(temp_str, cursor_x, cursor_y, 0xFF000000, 1);
                            }
                        }
                    }
                }

            }

            else if (win->type == WIN_TYPE_START_MENU) {
                render_start_menu(win, mouse_x, mouse_y);

            }

            else if (win->type == WIN_TYPE_APP_GUI) {
                render_app_gui(win, w, top_app_win, mouse_x, mouse_y, is_clicking);
            }
            else if (win->type == WIN_TYPE_APP_MENU) {
                render_app_menu(win, mouse_x, mouse_y);
            }

            // La finestra e' appena stata disegnata e nessuna di quelle sopra
            // lo e' ancora: e' il momento giusto per memorizzarla.
            if (use_cache) {
                cache_capture(win);
                win->cache_sig = wsig;
            }

            // Dallo studio al posto vero: si ripristina cio' che c'era sotto la
            // posizione di studio, e si copia la finestra dove deve stare
            if (studio) {
                win->x = real_x;
                win->y = real_y;
                mouse_x = real_mx;
                mouse_y = real_my;
                studio_restore();
                if (!win->is_maximized)
                    draw_window_shadow(win->x, win->y, win->w, win->h);
                cache_blit(win);
            }
            draw_live_controls(win, w, w == top_app_win, mouse_x, mouse_y, is_clicking);
            draw_window_overlays(win, w);
        }
    }

    // La barra dei menu resta sempre in vista: una finestra massimizzata si
    // ferma sotto di essa invece di coprirla.
    perf_mark(1);   // finestre
    draw_top_panel();
    draw_panel_menus(mouse_x, mouse_y);
    //draw_perf_counter(); //contatore dei tempi di disegno

    // I comandi della finestra massimizzata vivono nel pannello, quindi vanno
    // tracciati dopo di esso.
    if (top_app_win >= 0 && windows[top_app_win].active &&
        !windows[top_app_win].is_minimized && windows[top_app_win].is_maximized) {
        draw_window_controls(&windows[top_app_win], top_app_win, 1,
                             mouse_x, mouse_y, is_clicking);
    }


    // Vassoio della barra superiore. Ordine: particelle, poi mascotte, poi
    // vassoio. Le scintille restano dietro la testa, e il vassoio disegnato
    // per ultimo copre il taglio del corpo: cosi' sembra seduta nella conca
    // invece che mozzata a meta'.
    if (panel_tray_active(global_active_win)) {
        int tcx = panel_tray_cx();

        draw_tray_particles(tcx, PANEL_TRAY_BASE, 50);

        if (mascot_where() == 3 && !mascot_drag) {
            gfx_clip_y1 = PANEL_TRAY_BASE;
            draw_mascot(tcx - MASCOT_W / 2, PANEL_MASCOT_Y);
            gfx_clip_y1 = 1 << 30;
        }

        draw_tray(tcx, PANEL_TRAY_BASE, 0xFFC8C8C8, 0xFF6E6E6E);
    }

    perf_mark(2);   // barra superiore
    // Taskbar con orologio, info e bottoni delle app
    render_taskbar(mouse_x, mouse_y, is_clicking);
    perf_mark(3);   // taskbar


    // Nuvola con percorso dell'eseguibile  
    draw_resize_frame();
    render_tooltip();

    draw_mascot_tip();

    // Animazione di minimizzazione
    render_genie_animation();


    // Barra di caricamento
    render_progress_overlay();
    // La tendina copre tutto: esce per ultima
    draw_panel_dropdown(mouse_x, mouse_y);

    // La mascotte trascinata vola sopra ogni cosa, pannello compreso
    if (mascot_drag)
        draw_mascot(mouse_x - mascot_grab_dx, mouse_y - mascot_grab_dy);
    perf_mark(4);   // extra: nuvolette, animazioni, tendina, mascotte trascinata
    swap_buffers();
    perf_mark(5);   // swap
    perf_record(perf_t0, rdtsc64());

    // La freccia è rimasta sullo schermo durante lo swap.
    // La riposizioniamo solo se nel frattempo il mouse si è spostato.
    if (cursor_drawn_x != mouse_x || cursor_drawn_y != mouse_y) {
        cursor_move(mouse_x, mouse_y);
    }
}





// ==========================================
// MOUSE PS/2
// ==========================================
void mouse_wait(uint8_t type) {
    uint32_t timeout = 100000;
    if (type == 0) { while (timeout--) { if ((inb(0x64) & 1) == 1) return; } } 
    else { while (timeout--) { if ((inb(0x64) & 2) == 0) return; } }
}

void mouse_init() {
    mouse_wait(1); outb(0x64, 0xA8); 
    mouse_wait(1); outb(0x64, 0x20); 
    mouse_wait(0); uint8_t status = (inb(0x60) | 2); 
    mouse_wait(1); outb(0x64, 0x60); mouse_wait(1); outb(0x60, status);
    mouse_wait(1); outb(0x64, 0xD4); mouse_wait(1); outb(0x60, 0xF4); 
    mouse_wait(0); (void)inb(0x60); 
}


// ============================================================================
// MOUSE VIA INTERRUPT (IRQ12)
// Il controller PS/2 genera un interrupt per ogni byte del mouse. Il gestore
// lo mette subito in un buffer ad anello, e il ciclo principale lo consuma con
// calma. Prima i byte venivano letti solo fra un fotogramma e l'altro: durante
// un disegno lungo il controller, che ha un buffer di un solo byte, perdeva i
// successivi e il cursore si "trascinava". Ora nessun byte va perso, qualunque
// cosa stia facendo il sistema.
// Indici a 8 bit: arrivati a 255 ripartono da 0 da soli, quindi il buffer da
// 256 posti si avvolge senza nessun calcolo di modulo.
// ============================================================================
volatile uint8_t mouse_ring[256];
volatile uint8_t mouse_ring_head = 0;   // lo avanza l'interrupt
volatile uint8_t mouse_ring_tail = 0;   // lo avanza il ciclo principale

void mouse_irq_handler(void) {
    uint8_t st = inb(0x64);

    // Il byte e' davvero del mouse solo se il bit 5 e' acceso. Un tasto non va
    // toccato: e' della tastiera, che resta letta in polling.
    if ((st & 0x21) == 0x21) {
        uint8_t b = inb(0x60);
        uint8_t next = (uint8_t)(mouse_ring_head + 1);
        if (next != mouse_ring_tail) {          // buffer pieno: il byte si perde
            mouse_ring[mouse_ring_head] = b;
            mouse_ring_head = next;
        }
    }

    // Fine interrupt: prima al PIC secondario, a cui e' collegato il mouse,
    // poi al primario, attraverso cui il secondario arriva alla CPU.
    outb(0xA0, 0x20);
    outb(0x20, 0x20);
}

// Stessa struttura del gestore del timer, ma senza cambio di processo
__attribute__((naked)) void isr44_mouse() {
    asm volatile(
        "pushal \n"
        "push %ds \n"
        "push %es \n"
        "push %fs \n"
        "push %gs \n"
        "call mouse_irq_handler \n"
        "pop %gs \n"
        "pop %fs \n"
        "pop %es \n"
        "pop %ds \n"
        "popal \n"
        "iret \n"
    );
}

// ==========================================
// DRIVER DISCO FISSO (ATA PIO MODE Sicuro)
// ==========================================
// Ritorna 1 se pronto, 0 se errore o nessun disco
int ata_wait_ready() {
    int timeout = 100000;
    while (--timeout) {
        uint8_t status = inb(0x1F7);
        // Se la porta risponde 0xFF significa che non c'è nessun disco attaccato
        if (status == 0xFF) return 0; 
        // BSY=0 e RDY=1: Il disco è pronto per parlare
        if ((status & 0xC0) == 0x40) return 1; 
    }
    return 0; // Timeout: il disco si è bloccato
}


// Interroga l'Hardware per scoprire Modello e Capienza del disco
int ata_identify() {
    // 1. Selezioniamo il disco primario (Master)
    outb(0x1F6, 0xA0); 
    
    // 2. Azzeriamo i parametri come richiesto dallo standard ATA
    outb(0x1F2, 0); outb(0x1F3, 0); outb(0x1F4, 0); outb(0x1F5, 0);
    
    // 3. Inviamo il comando IDENTIFY (0xEC)
    outb(0x1F7, 0xEC);

    // Leggiamo lo stato. Se è 0, non c'è nessun disco collegato
    uint8_t status = inb(0x1F7);
    if (status == 0) return 0; 

    // 4. Aspettiamo che il disco smetta di essere occupato (BSY)
    while ((inb(0x1F7) & 0x80) != 0);

    // 5. Verifichiamo se ha risposto DRQ (Data Request) o ERR (Error)
    while (1) {
        status = inb(0x1F7);
        if (status & 0x01) return 0; // Errore! Forse non è un disco ATA
        if (status & 0x08) break;    // Dati pronti per essere letti!
    }

    // 6. Leggiamo i 256 pacchetti da 16-bit (512 byte in totale)
    uint16_t identify_data[256] = {0}; 
    for (int i = 0; i < 256; i++) {
        identify_data[i] = inw(0x1F0);
    }

    // 7. ESTRAZIONE DATI: Settori Totali (Supporto LBA48 fino a 2 Terabyte)
    // Controlliamo se il disco supporta LBA48 leggendo il bit 10 della parola 83
    if (identify_data[83] & (1 << 10)) {
        // LBA48: Leggiamo le parole 100 e 101
        ata_drive_capacity_sectors = ((uint32_t)identify_data[101] << 16) | (uint32_t)identify_data[100];
    } else {
        // Fallback per dischi antichi: Vecchio LBA28
        ata_drive_capacity_sectors = ((uint32_t)identify_data[61] << 16) | (uint32_t)identify_data[60];
    }
    
    // (Settori / 2048) evita l'overflow a 32-bit dei dischi enormi
    ata_drive_capacity_mb = ata_drive_capacity_sectors / 2048;

    // 8. ESTRAZIONE DATI: Modello del disco (Dalla parola 27 alla 46)
    // Lo standard ATA salva le lettere invertite a coppie! Dobbiamo swapparle.
    int k = 0;
    for (int i = 27; i <= 46; i++) {
        ata_drive_model[k++] = (char)(identify_data[i] >> 8);   // Lettera a Sinistra
        ata_drive_model[k++] = (char)(identify_data[i] & 0xFF); // Lettera a Destra
    }
    ata_drive_model[40] = '\0';

    // Puliamo gli spazi vuoti alla fine del nome
    for (int i = 39; i >= 0; i--) {
        if (ata_drive_model[i] == ' ') ata_drive_model[i] = '\0';
        else break;
    }

    return 1;
}

// Ritorna 1 se ha letto con successo, 0 altrimenti
int ata_read_sector(uint32_t lba, uint8_t* buffer) {
    // Se superiamo i 137 GB (0x0FFFFFFF), accendiamo la trazione integrale LBA48
    int use_lba48 = (lba >= 0x0FFFFFFF);

    // 1. Selezioniamo il disco
    if (use_lba48) {
        outb(0x1F6, 0x40); // LBA48 non usa i bit alti del LBA in questa porta
    } else {
        outb(0x1F6, 0xE0 | ((lba >> 24) & 0x0F)); 
    }
    
    inb(0x1F7); inb(0x1F7); inb(0x1F7); inb(0x1F7); // Ritardo
    if (!ata_wait_ready()) return 0;
    
    // 2. Invio dei byte alti (Solo per LBA48)
    if (use_lba48) {
        outb(0x1F2, 0); // Settori alti (0 perché leggiamo 1 solo settore)
        outb(0x1F3, (uint8_t)(lba >> 24)); // LBA4
        outb(0x1F4, 0); // LBA5 (Zero finché stiamo sotto i 2TB)
        outb(0x1F5, 0); // LBA6
    }
    
    // 3. Settiamo i parametri base (Per entrambi)
    outb(0x1F2, 1); // Settori bassi (Vogliamo leggere 1 settore)
    outb(0x1F3, (uint8_t)lba);
    outb(0x1F4, (uint8_t)(lba >> 8));
    outb(0x1F5, (uint8_t)(lba >> 16));
    
    // 4. Invia comando READ (Standard o Extended)
    if (use_lba48) outb(0x1F7, 0x24); // READ SECTORS EXT
    else outb(0x1F7, 0x20);           // READ SECTORS
    
    // 5. Aspettiamo che ci dica DRQ (Data Request)
    int timeout = 100000;

    while (--timeout) {
        uint8_t status = inb(0x1F7);
        if (status & 0x08) break; // Dati pronti
        if (status & 0x01) return 0; // Errore hardware
    }
    if (timeout == 0) return 0;

    // 6. Leggi i dati
    uint16_t* ptr = (uint16_t*)buffer;
    for (int i = 0; i < 256; i++) {
        ptr[i] = inw(0x1F0);
    }

    if (show_progress_bar && progress_total > 0) {
        progress_current += 512;
        if (progress_current > progress_total) progress_current = progress_total;
        
        // Aggiorna la grafica solo ogni 64 KB (65536 byte) trasferiti
        if (progress_current % 65536 == 0) render_desktop(mouse_x, mouse_y, is_clicking);
    }

    return 1; // Successo
}

// Ritorna 1 se ha scritto con successo, 0 altrimenti
int ata_write_sector(uint32_t lba, uint8_t* buffer) {
    int use_lba48 = (lba >= 0x0FFFFFFF);

    if (use_lba48) outb(0x1F6, 0x40); 
    else outb(0x1F6, 0xE0 | ((lba >> 24) & 0x0F)); 
    
    inb(0x1F7); inb(0x1F7); inb(0x1F7); inb(0x1F7); 
    if (!ata_wait_ready()) return 0;

    if (use_lba48) {
        outb(0x1F2, 0); 
        outb(0x1F3, (uint8_t)(lba >> 24)); 
        outb(0x1F4, 0); outb(0x1F5, 0);
    }
    
    outb(0x1F2, 1);
    outb(0x1F3, (uint8_t)lba);
    outb(0x1F4, (uint8_t)(lba >> 8));
    outb(0x1F5, (uint8_t)(lba >> 16));
    
    // Comando "WRITE SECTORS" (0x30) o "EXT" (0x34)
    if (use_lba48) outb(0x1F7, 0x34);
    else outb(0x1F7, 0x30);
    
    // Aspettiamo che ci chieda i dati (DRQ bit = 1)
    int timeout = 100000;

    while (--timeout) {
        uint8_t status = inb(0x1F7);
        if (status & 0x08) break; 
        if (status & 0x01) return 0; 
    }
    if (timeout == 0) return 0;

    // SCRIVIAMO I DATI (256 istruzioni da 16-bit = 512 byte)
    uint16_t* ptr = (uint16_t*)buffer;
    for (int i = 0; i < 256; i++) {
        outw(0x1F0, ptr[i]);
    }

    // Svuotiamo la cache del disco per sicurezza (Cache Flush = 0xE7)
    outb(0x1F7, 0xE7);
    ata_wait_ready();

    if (show_progress_bar && progress_total > 0) {
        progress_current += 512;
        if (progress_current > progress_total) progress_current = progress_total;
        
        if (progress_current % 65536 == 0) render_desktop(mouse_x, mouse_y, is_clicking);
    }
    return 1;
}


// ==========================================
// MOUNTING DEL FILE SYSTEM
// ==========================================
// Legge il Superblocco e calcola la divisione in "Quartieri" (Block Groups)
int ext2_mount() {
    uint8_t sector_buffer[512] = {0};
    
    // Il Superblocco inizia SEMPRE al byte 1024 del disco (LBA 2)
    if (!ata_read_sector(2, sector_buffer)) return 0;
    
    ext2_superblock_t sb;
    uint8_t* dst = (uint8_t*)&sb;
    for(size_t i = 0; i < sizeof(ext2_superblock_t); i++) dst[i] = sector_buffer[i];
    
    if (sb.s_magic != 0xEF53) return 0; // Se non ha la firma magica, non è Ext2
    
    ext2_blocks_per_group = sb.s_blocks_per_group;
    ext2_inodes_per_group = sb.s_inodes_per_group;
    
    // Calcoliamo quanti "Quartieri" ci sono arrotondando per eccesso
    ext2_total_groups = (sb.s_blocks_count + ext2_blocks_per_group - 1) / ext2_blocks_per_group;
    
    // Per blocchi da 1024 byte, la Tabella dei Quartieri (BGDT) inizia nel Blocco 2 (LBA 4)
    ext2_bgdt_lba = 4; 
    
    return 1;
}


// ==========================================
// MOTORE DI SCRITTURA EXT2
// ==========================================

// Trova e prenota un Blocco Dati libero viaggiando su tutto il disco
uint32_t ext2_allocate_block() {
    uint8_t sector_buffer[512] = {0};
    uint8_t bitmap_buffer[512] = {0};

    // Cicliamo su TUTTI i Block Groups
    for (uint32_t bg = 0; bg < ext2_total_groups; bg++) {
        
        // 1. Troviamo il descrittore di QUESTO specifico gruppo nella BGDT
        // Ogni descrittore pesa 32 byte (in un settore da 512 ce ne stanno 16)
        uint32_t bgdt_sector = ext2_bgdt_lba + ((bg * 32) / 512);
        uint32_t bgdt_offset = (bg * 32) % 512;
        
        if (!ata_read_sector(bgdt_sector, sector_buffer)) continue;
        
        ext2_bg_desc_t bgd;
        uint8_t* dst = (uint8_t*)&bgd;
        for(size_t i=0; i<sizeof(ext2_bg_desc_t); i++) dst[i] = sector_buffer[bgdt_offset + i];

        // 2. Troviamo la mappa dei blocchi di questo gruppo
        uint32_t bitmap_lba = bgd.bg_block_bitmap * 2;

        // Un gruppo ha migliaia di blocchi, la sua bitmap occupa più di un settore!
        uint32_t bitmap_sectors = (ext2_blocks_per_group + 4095) / 4096; 
        
        // Scansioniamo tutti i settori della bitmap di questo gruppo
        for (uint32_t s = 0; s < bitmap_sectors; s++) {
            if (!ata_read_sector(bitmap_lba + s, bitmap_buffer)) continue;

            for (uint32_t byte_idx = 0; byte_idx < 512; byte_idx++) {
                if (bitmap_buffer[byte_idx] != 0xFF) { // C'è spazio
                    for (int bit = 0; bit < 8; bit++) {
                        if (!(bitmap_buffer[byte_idx] & (1 << bit))) {
                            
                            // TROVATO Lo marchiamo occupato e salviamo
                            bitmap_buffer[byte_idx] |= (1 << bit);
                            ata_write_sector(bitmap_lba + s, bitmap_buffer);

                            // Calcoliamo il numero finale del blocco
                            // Blocco = (N. Gruppo * Blocchi per Gruppo) + (Offset nella mappa) + 1
                            uint32_t block_num = (bg * ext2_blocks_per_group) + (s * 4096) + (byte_idx * 8) + bit + 1;
                            return block_num;
                        }
                    }
                }
            }
        }
    }
    return 0; // L'intero disco è completamente pieno
}

// Trova e prenota un Inode libero viaggiando su tutto il disco
uint32_t ext2_allocate_inode() {
    uint8_t sector_buffer[512] = {0};
    uint8_t bitmap_buffer[512] = {0};

    for (uint32_t bg = 0; bg < ext2_total_groups; bg++) {
        uint32_t bgdt_sector = ext2_bgdt_lba + ((bg * 32) / 512);
        uint32_t bgdt_offset = (bg * 32) % 512;
        
        if (!ata_read_sector(bgdt_sector, sector_buffer)) continue;
        
        ext2_bg_desc_t bgd;
        uint8_t* dst = (uint8_t*)&bgd;
        for(size_t i=0; i<sizeof(ext2_bg_desc_t); i++) dst[i] = sector_buffer[bgdt_offset + i];

        uint32_t bitmap_lba = bgd.bg_inode_bitmap * 2;
        uint32_t bitmap_sectors = (ext2_inodes_per_group + 4095) / 4096; 
        
        for (uint32_t s = 0; s < bitmap_sectors; s++) {
            if (!ata_read_sector(bitmap_lba + s, bitmap_buffer)) continue;

            for (uint32_t byte_idx = 0; byte_idx < 512; byte_idx++) {
                if (bitmap_buffer[byte_idx] != 0xFF) { 
                    for (int bit = 0; bit < 8; bit++) {
                        if (!(bitmap_buffer[byte_idx] & (1 << bit))) {
                            
                            bitmap_buffer[byte_idx] |= (1 << bit);
                            ata_write_sector(bitmap_lba + s, bitmap_buffer);

                            uint32_t inode_num = (bg * ext2_inodes_per_group) + (s * 4096) + (byte_idx * 8) + bit + 1;
                            return inode_num;
                        }
                    }
                }
            }
        }
    }
    return 0; 
}

// ==============================================================================
// VFS CORE: TRADUTTORE DI BLOCCHI E GESTIONE INODE
// ==============================================================================

// Helper: Estrae la struttura Inode completa direttamente dal disco
ext2_inode_t ext2_get_inode_struct(uint32_t inode_num) {
    ext2_inode_t inode; uint8_t* out = (uint8_t*)&inode;
    for(size_t i=0; i<sizeof(ext2_inode_t); i++) out[i] = 0;
    if (inode_num == 0) return inode;

    uint8_t sector_buffer[512];
    ata_read_sector(4, sector_buffer);
    ext2_bg_desc_t bgd; uint8_t* dst = (uint8_t*)&bgd;
    for(size_t i=0; i<sizeof(ext2_bg_desc_t); i++) dst[i] = sector_buffer[i];
    
    uint32_t inode_table_lba = bgd.bg_inode_table * 2;
    uint32_t inode_size = 128;
    
    ata_read_sector(inode_table_lba, sector_buffer);
    ext2_inode_t root_test; dst = (uint8_t*)&root_test;
    for(size_t i=0; i<sizeof(ext2_inode_t); i++) dst[i] = sector_buffer[128 + i];
    if ((root_test.i_mode & 0xF000) != 0x4000) inode_size = 256;

    uint32_t index = inode_num - 1;
    ata_read_sector(inode_table_lba + ((index * inode_size) / 512), sector_buffer);
    for(size_t i=0; i<sizeof(ext2_inode_t); i++) out[i] = sector_buffer[((index * inode_size) % 512) + i];

    return inode;
}

// IL MOTORE VFS: Traduce un blocco logico (es. il 15° Kilobyte del file) nel vero Blocco Fisico Ext2 
uint32_t ext2_get_phys_block(ext2_inode_t* inode, uint32_t logical_block) {
    // 1. Blocchi Diretti
    if (logical_block < 12) return inode->i_block[logical_block];
    
    // 2. Singolo Indiretto
    if (logical_block < 268) {
        uint8_t sec[1024];
        ata_read_sector(inode->i_block[12] * 2, sec);
        ata_read_sector(inode->i_block[12] * 2 + 1, sec + 512);
        return ((uint32_t*)sec)[logical_block - 12];
    }
    
    // 3. Doppio Indiretto
    if (logical_block < 65804) {
        uint32_t d_idx = logical_block - 268;
        uint8_t sec[1024];
        ata_read_sector(inode->i_block[13] * 2, sec);
        ata_read_sector(inode->i_block[13] * 2 + 1, sec + 512);
        uint32_t sub_block = ((uint32_t*)sec)[d_idx / 256]; // Troviamo la sub-rubrica
        
        ata_read_sector(sub_block * 2, sec);
        ata_read_sector(sub_block * 2 + 1, sec + 512);
        return ((uint32_t*)sec)[d_idx % 256]; // Estraiamo il blocco finale
    }
    return 0;
}

// ==============================================================================
// IL PATH PARSER: Naviga l'albero delle directory e trova l'Inode di destinazione
// ==============================================================================
uint32_t ext2_resolve_path(const char* path) {
    // 1. Controllo di base: accettiamo solo percorsi assoluti che partono dalla Root
    if (path[0] != '/') return 0; 

    uint8_t sector_buffer[512] = {0};

    // 2. Leggiamo la BGDT per sapere dove si trova la Tabella degli Inode
    if (!ata_read_sector(4, sector_buffer)) return 0;
    ext2_bg_desc_t bgd; uint8_t* dst = (uint8_t*)&bgd;
    for(size_t i=0; i<sizeof(ext2_bg_desc_t); i++) dst[i] = sector_buffer[i];
    uint32_t inode_table_lba = bgd.bg_inode_table * 2;

    // Scopriamo la grandezza degli Inode formattati (128 o 256 byte)
    uint32_t inode_size = 128;
    ata_read_sector(inode_table_lba, sector_buffer);
    ext2_inode_t root_test; dst = (uint8_t*)&root_test;
    for(size_t i=0; i<sizeof(ext2_inode_t); i++) dst[i] = sector_buffer[128 + i];
    if ((root_test.i_mode & 0xF000) != 0x4000) inode_size = 256;

    // 3. INIZIO DEL VIAGGIO: Partiamo sempre dalla Root (Inode 2)
    uint32_t current_inode_num = 2; 
    int p_idx = 1; // Iniziamo a leggere la stringa dal carattere 1 (saltiamo il primo '/')

    while (path[p_idx] != '\0') {
        // A. Estraiamo la prossima cartella/file dal percorso (fino al prossimo slash)
        char target_name[256];
        int t_len = 0;
        while (path[p_idx] != '/' && path[p_idx] != '\0' && t_len < 255) {
            target_name[t_len++] = path[p_idx++];
        }
        target_name[t_len] = '\0';

        // (Sicurezza: Se troviamo due slash di fila come "//", li ignoriamo)
        if (t_len == 0) {
            if (path[p_idx] == '/') p_idx++;
            continue;
        }

        // B. Apriamo in RAM l'Inode Corrente
        uint32_t index = current_inode_num - 1;
        uint32_t sect_off = (index * inode_size) / 512;
        uint32_t in_sect_off = (index * inode_size) % 512;

        if (!ata_read_sector(inode_table_lba + sect_off, sector_buffer)) return 0;
        
        ext2_inode_t current_inode; dst = (uint8_t*)&current_inode;
        for(size_t i=0; i<sizeof(ext2_inode_t); i++) dst[i] = sector_buffer[in_sect_off + i];

        // C. L'Inode in cui siamo entrati è davvero una cartella (0x4000)?
        if ((current_inode.i_mode & 0xF000) != 0x4000) return 0; 




        // D. Apriamo il blocco dati della cartella e cerchiamo "target_name"
        uint32_t dir_block_lba = current_inode.i_block[0] * 2;
        uint8_t dir_buffer[1024]; // Nuovo buffer capiente
        if (!ata_read_sector(dir_block_lba, dir_buffer)) return 0;
        if (!ata_read_sector(dir_block_lba + 1, dir_buffer + 512)) return 0;

        uint32_t offset = 0;
        uint32_t next_inode_num = 0;

        while (offset < 1024) { // Scansioniamo tutti i 1024 byte
            ext2_dir_entry_t entry; uint8_t* edst = (uint8_t*)&entry;
            for(size_t i = 0; i < sizeof(ext2_dir_entry_t); i++) {edst[i] = dir_buffer[offset + i];}
            
            if (entry.inode == 0 || entry.rec_len == 0) break;

            char fname[256];
            for (int k = 0; k < entry.name_len; k++) {fname[k] = dir_buffer[offset + 8 + k];}
            
            fname[entry.name_len] = '\0';

            // Confrontiamo i nomi
            int match = 1;
            for(int k = 0; k <= entry.name_len; k++) {
                if (fname[k] != target_name[k]) { match = 0; break; }
            }

            // Memorizziamo l'Inode del prossimo passo e rompiamo il ciclo
            if (match) {
                next_inode_num = entry.inode;
                break; 
            }
            offset += entry.rec_len; // Saltiamo al prossimo file nella cartella
        }

        // E. Il file/cartella non esiste
        if (next_inode_num == 0) return 0; 

        // F. Preparazione per il prossimo giro: saltiamo dentro la nuova cartella
        current_inode_num = next_inode_num;
        if (path[p_idx] == '/') p_idx++; // Saltiamo lo slash separatore
    }

    // 4. ARRIVO: Se siamo usciti dal ciclo sani e salvi, abbiamo in mano la destinazione!
    return current_inode_num;
}



// Controlla se un file o cartella esiste già nella Root (Ritorna 1 se esiste, 0 se libero)
int ext2_exists(const char* target_name) {
    uint8_t sector_buffer[512] = {0};
    if (!ata_read_sector(4, sector_buffer)) return 0;
    
    ext2_bg_desc_t bgd; uint8_t* dst = (uint8_t*)&bgd;
    for(size_t i=0; i<sizeof(ext2_bg_desc_t); i++) dst[i] = sector_buffer[i];

    uint32_t inode_table_lba = bgd.bg_inode_table * 2;
    if (!ata_read_sector(inode_table_lba, sector_buffer)) return 0;

    ext2_inode_t root_inode; dst = (uint8_t*)&root_inode;
    
    // Proviamo a leggere l'Inode 2 assumendo che la grandezza sia 128 byte
    for(size_t i=0; i<128; i++) dst[i] = sector_buffer[128 + i];
    
    // Se non è una cartella (0x4000), significa che mke2fs ha formattato a 256 byte
    if ((root_inode.i_mode & 0xF000) != 0x4000) {
        for(size_t i=0; i<256; i++) dst[i] = sector_buffer[256 + i];
    }

    if ((root_inode.i_mode & 0xF000) == 0x4000) {
        uint8_t dir_buffer[1024]; 
        if (ata_read_sector(root_inode.i_block[0] * 2, dir_buffer) && 
            ata_read_sector((root_inode.i_block[0] * 2) + 1, dir_buffer + 512)) {
            
            uint32_t offset = 0;
            while (offset < 1024) { // Scansioniamo tutto
                ext2_dir_entry_t entry; uint8_t* edst = (uint8_t*)&entry;
                for(size_t i = 0; i < sizeof(ext2_dir_entry_t); i++) {edst[i] = dir_buffer[offset + i];}
                if (entry.inode == 0 || entry.rec_len == 0) break;

                char fname[256];
                for (int k = 0; k < entry.name_len; k++) {fname[k] = dir_buffer[offset + 8 + k];}
                
                
                fname[entry.name_len] = '\0';

                int match = 1;
                for(int k = 0; k <= entry.name_len; k++) {
                    if (fname[k] != target_name[k]) { match = 0; break; }
                }
                if (match) return 1; // IL NOME è GIà PRESO

                offset += entry.rec_len;
            }
        }
    }
    return 0; // Il nome è libero
}


// Scrive un nuovo file di testo nel percorso specificato
int ext2_write_file(const char* full_path, const char* data, uint32_t file_size) {
    // 1. SPLIT DEL PERCORSO: Separare cartella madre e nome file
    char parent_path[256];
    char filename[256];
    int last_slash = -1;
    int len = 0;
    
    while(full_path[len] != '\0' && len < 255) {
        if (full_path[len] == '/') last_slash = len;
        len++;
    }
    if (last_slash == -1 || len == 1) return 0; // Percorso non valido
    
    // Estraiamo la cartella madre
    if (last_slash == 0) { 
        parent_path[0] = '/'; parent_path[1] = '\0';
    } else {
        for(int i=0; i<last_slash; i++) parent_path[i] = full_path[i];
        parent_path[last_slash] = '\0';
    }
    
    // Estraiamo il nome del file
    int f_idx = 0;
    for(int i=last_slash+1; i<len; i++) filename[f_idx++] = full_path[i];
    filename[f_idx] = '\0';
    if (f_idx == 0) return 0; // Nessun nome file

    // 2. RISOLUZIONE DELLA CARTELLA MADRE
    uint32_t parent_inode_num = ext2_resolve_path(parent_path);
    if (parent_inode_num == 0) return -2; // Errore: Cartella madre inesistente

    uint8_t sector_buffer[512] = {0};
    uint8_t dir_buffer[1024] = {0};

    // Lettura BGDT
    if (!ata_read_sector(4, sector_buffer)) return 0;
    ext2_bg_desc_t bgd; uint8_t* dst = (uint8_t*)&bgd;
    for(size_t i=0; i<sizeof(ext2_bg_desc_t); i++) dst[i] = sector_buffer[i];
    uint32_t inode_table_lba = bgd.bg_inode_table * 2;
    
    uint32_t inode_size = 128;
    ata_read_sector(inode_table_lba, sector_buffer);
    ext2_inode_t root_test; dst = (uint8_t*)&root_test;
    for(size_t i=0; i<sizeof(ext2_inode_t); i++) dst[i] = sector_buffer[128 + i];
    if ((root_test.i_mode & 0xF000) != 0x4000) inode_size = 256;

    // 3. LEGGIAMO LA CARTELLA MADRE E CERCHIAMO DUPLICATI
    uint32_t p_index = parent_inode_num - 1;
    uint32_t p_sect_off = (p_index * inode_size) / 512;
    uint32_t p_in_sect_off = (p_index * inode_size) % 512;

    ata_read_sector(inode_table_lba + p_sect_off, sector_buffer);
    ext2_inode_t parent_inode;
    for(size_t i=0; i<sizeof(ext2_inode_t); i++) ((uint8_t*)&parent_inode)[i] = sector_buffer[p_in_sect_off + i];
    
    if ((parent_inode.i_mode & 0xF000) != 0x4000) return -2; // Non è una cartella

    uint32_t dir_block_lba = parent_inode.i_block[0] * 2;
    ata_read_sector(dir_block_lba, dir_buffer);
    ata_read_sector(dir_block_lba + 1, dir_buffer + 512);

    uint32_t offset = 0;
    uint32_t insert_offset = 0;
    uint32_t real_len_to_shrink = 0;

    while (offset < 1024) {
        ext2_dir_entry_t entry;
        for(size_t i=0; i<sizeof(ext2_dir_entry_t); i++) ((uint8_t*)&entry)[i] = dir_buffer[offset + i];
        if (entry.inode == 0 || entry.rec_len == 0) break;

        char fname[256];
        for(int k=0; k<entry.name_len; k++) fname[k] = dir_buffer[offset + 8 + k];
        fname[entry.name_len] = '\0';

        // Controllo Duplicato in questa cartella
        int match = 1;
        if (entry.name_len != f_idx) match = 0;
        else {
            for(int k=0; k<f_idx; k++) { if (fname[k] != filename[k]) { match = 0; break; } }
        }
        if (match) return -1; // DUPLICATO

        uint32_t real_len = 8 + entry.name_len;
        if (real_len % 4 != 0) real_len += 4 - (real_len % 4);

        if (entry.rec_len > real_len + 16) {
            insert_offset = offset;
            real_len_to_shrink = real_len;
        }
        offset += entry.rec_len;
    }

    if (insert_offset == 0 && offset > 0) return 0; // Spazio esaurito


    // 4. CREAZIONE FISICA E ALLOCAZIONE (Sblocco DOPPIO INDIRETTO: ~64 MB)
    if (file_size == 0) file_size = 1; 
    
    uint32_t MAX_EXT2_SIZE = 67383296;
    if (file_size > MAX_EXT2_SIZE) file_size = MAX_EXT2_SIZE;

    uint32_t num_blocks = (file_size + 1023) / 1024;
    uint32_t direct_count = (num_blocks > 12) ? 12 : num_blocks;
    uint32_t single_count = (num_blocks > 12) ? ((num_blocks - 12 > 256) ? 256 : num_blocks - 12) : 0;
    uint32_t double_count = (num_blocks > 268) ? (num_blocks - 268) : 0;

    uint32_t direct_blocks[12] = {0};
    uint32_t single_indirect_list_block = 0;
    uint32_t double_indirect_list_block = 0;

    // PREVENZIONE STACK OVERFLOW: Allochiamo i grandi buffer nell'Heap
    uint8_t* io_buf = (uint8_t*)kmalloc(1024);
    uint32_t* single_pointers = (uint32_t*)kmalloc(1024);
    uint32_t* double_master_pointers = (uint32_t*)kmalloc(1024);
    uint32_t* sub_pointers = (uint32_t*)kmalloc(1024);

    if (!io_buf || !single_pointers || !double_master_pointers || !sub_pointers) {
        if(io_buf) {kfree(io_buf);}
        if(single_pointers) {kfree(single_pointers);}
        if(double_master_pointers) {kfree(double_master_pointers);}
        if(sub_pointers) {kfree(sub_pointers);}
        return 0; // RAM esaurita
    }
    for(int i=0; i<256; i++) { single_pointers[i]=0; double_master_pointers[i]=0; sub_pointers[i]=0; }

    // A. ALLOCAZIONE BLOCCHI DIRETTI
    for (uint32_t b = 0; b < direct_count; b++) {
        direct_blocks[b] = ext2_allocate_block();
        if (direct_blocks[b] == 0) break; // Disco Pieno
        uint32_t offset = b * 1024;
        for(int i = 0; i < 1024; i++) {io_buf[i] = (data && offset + i < file_size) ? data[offset + i] : 0;}
        ata_write_sector(direct_blocks[b] * 2, io_buf);
        ata_write_sector((direct_blocks[b] * 2) + 1, io_buf + 512);
    }

    // B. ALLOCAZIONE SINGOLO INDIRETTO
    if (single_count > 0) {
        single_indirect_list_block = ext2_allocate_block();
        for (uint32_t b = 0; b < single_count; b++) {
            single_pointers[b] = ext2_allocate_block();
            uint32_t offset = (12 + b) * 1024;
            for(int i = 0; i < 1024; i++) {io_buf[i] = (data && offset + i < file_size) ? data[offset + i] : 0;}
            ata_write_sector(single_pointers[b] * 2, io_buf);
            ata_write_sector((single_pointers[b] * 2) + 1, io_buf + 512);
        }
        ata_write_sector(single_indirect_list_block * 2, (uint8_t*)single_pointers);
        ata_write_sector((single_indirect_list_block * 2) + 1, ((uint8_t*)single_pointers) + 512);
    }

    // C. ALLOCAZIONE DOPPIO INDIRETTO
    if (double_count > 0) {
        double_indirect_list_block = ext2_allocate_block();
        uint32_t blocks_written = 0;
        uint32_t master_idx = 0;

        while (blocks_written < double_count && master_idx < 256) {
            double_master_pointers[master_idx] = ext2_allocate_block();
            for(int i=0; i<256; i++) sub_pointers[i] = 0; 
            
            uint32_t sub_idx = 0;
            while (sub_idx < 256 && blocks_written < double_count) {
                sub_pointers[sub_idx] = ext2_allocate_block();
                uint32_t offset = (268 + blocks_written) * 1024;
                for(int i = 0; i < 1024; i++) {io_buf[i] = (data && offset + i < file_size) ? data[offset + i] : 0;}
                ata_write_sector(sub_pointers[sub_idx] * 2, io_buf);
                ata_write_sector((sub_pointers[sub_idx] * 2) + 1, io_buf + 512);
                
                sub_idx++;
                blocks_written++;
            }
            ata_write_sector(double_master_pointers[master_idx] * 2, (uint8_t*)sub_pointers);
            ata_write_sector((double_master_pointers[master_idx] * 2) + 1, ((uint8_t*)sub_pointers) + 512);
            master_idx++;
        }
        ata_write_sector(double_indirect_list_block * 2, (uint8_t*)double_master_pointers);
        ata_write_sector((double_indirect_list_block * 2) + 1, ((uint8_t*)double_master_pointers) + 512);
    }

    // PULIZIA RAM KERNEL
    kfree(io_buf); kfree(single_pointers); kfree(double_master_pointers); kfree(sub_pointers);



    uint32_t new_inode = ext2_allocate_inode();
    if (new_inode == 0) return 0;

    // 5. CARTA D'IDENTITà (INODE) DEL NUOVO FILE MULTI-BLOCCO
    uint32_t n_index = new_inode - 1;
    uint32_t n_sect_off = (n_index * inode_size) / 512;
    uint32_t n_in_sect_off = (n_index * inode_size) % 512;

    ata_read_sector(inode_table_lba + n_sect_off, sector_buffer);
    
    ext2_inode_t new_file_inode;
    for(size_t i = 0; i < sizeof(ext2_inode_t); i++) ((uint8_t*)&new_file_inode)[i] = 0;
    new_file_inode.i_mode = 0x81A4; 
    new_file_inode.i_size = file_size;   
    new_file_inode.i_links_count = 1; 
    
    // Calcoliamo l'ingombro reale in settori sul disco (inclusi i blocchi rubrica)
    uint32_t meta_blocks = 0;
    if (single_count > 0) meta_blocks += 1;
    if (double_count > 0) {
        meta_blocks += 1; // La Master-Rubrica
        meta_blocks += ((double_count + 255) / 256); // Le Sub-Rubriche
    }
    
    new_file_inode.i_blocks = (num_blocks * 2) + (meta_blocks * 2); 
    
    // Agganciamo i blocchi all'Inode
    for(uint32_t b = 0; b < direct_count; b++) new_file_inode.i_block[b] = direct_blocks[b];
    if (single_count > 0) new_file_inode.i_block[12] = single_indirect_list_block;
    if (double_count > 0) new_file_inode.i_block[13] = double_indirect_list_block;





    for(size_t i = 0; i < sizeof(ext2_inode_t); i++) sector_buffer[n_in_sect_off + i] = ((uint8_t*)&new_file_inode)[i];
    ata_write_sector(inode_table_lba + n_sect_off, sector_buffer);

    // 6. AGGIUNTA ALLA CARTELLA MADRE
    ext2_dir_entry_t prev_entry;
    for(size_t i=0; i<sizeof(ext2_dir_entry_t); i++) ((uint8_t*)&prev_entry)[i] = dir_buffer[insert_offset + i];
    
    uint16_t old_rec_len = prev_entry.rec_len;
    prev_entry.rec_len = real_len_to_shrink;
    for(size_t i=0; i<sizeof(ext2_dir_entry_t); i++) dir_buffer[insert_offset + i] = ((uint8_t*)&prev_entry)[i];
    
    insert_offset += real_len_to_shrink;
    
    ext2_dir_entry_t new_entry;
    new_entry.inode = new_inode;
    new_entry.rec_len = old_rec_len - real_len_to_shrink;
    new_entry.name_len = f_idx;
    new_entry.file_type = 1; // 1 = FILE
    
    for(size_t i=0; i<sizeof(ext2_dir_entry_t); i++) dir_buffer[insert_offset + i] = ((uint8_t*)&new_entry)[i];
    for(int i=0; i<f_idx; i++) dir_buffer[insert_offset + 8 + i] = filename[i]; 
    
    ata_write_sector(dir_block_lba, dir_buffer);

    return 1; 
}



// Crea una nuova Directory seguendo il percorso! (Es: "/system/apps_data")
int ext2_make_directory(const char* full_path) {
    // 1. SPLIT DEL PERCORSO: Separare cartella madre e nuovo nome
    char parent_path[256];
    char new_dirname[256];
    int last_slash = -1;
    int len = 0;
    
    while(full_path[len] != '\0' && len < 255) {
        if (full_path[len] == '/') last_slash = len;
        len++;
    }
    if (last_slash == -1 || len == 1) return 0; // Errore: percorso non valido o solo '/'
    
    // Estraiamo la cartella madre
    if (last_slash == 0) { // Es: "/apps" -> Madre = "/"
        parent_path[0] = '/'; parent_path[1] = '\0';
    } else {
        for(int i=0; i<last_slash; i++) parent_path[i] = full_path[i];
        parent_path[last_slash] = '\0';
    }
    
    // Estraiamo il nuovo nome
    int d_idx = 0;
    for(int i=last_slash+1; i<len; i++) new_dirname[d_idx++] = full_path[i];
    new_dirname[d_idx] = '\0';
    if (d_idx == 0) return 0; // Nessun nome fornito

    // 2. RISOLUZIONE DELLA CARTELLA MADRE
    uint32_t parent_inode_num = 2; // Root di default
    if (parent_path[0] == '/' && parent_path[1] == '\0') {
        parent_inode_num = 2;
    } else {
        parent_inode_num = ext2_resolve_path(parent_path);
        if (parent_inode_num == 0) return -2; // Errore: Cartella madre inesistente!
    }

    uint8_t sector_buffer[512] = {0};
    uint8_t dir_buffer[1024] = {0}; // Usiamo un buffer separato per i dati della cartella

    // Leggiamo la BGDT
    if (!ata_read_sector(4, sector_buffer)) return 0;
    ext2_bg_desc_t bgd; uint8_t* dst = (uint8_t*)&bgd;
    for(size_t i=0; i<sizeof(ext2_bg_desc_t); i++) dst[i] = sector_buffer[i];
    uint32_t inode_table_lba = bgd.bg_inode_table * 2;
    
    // Grandezza Inode
    uint32_t inode_size = 128;
    ata_read_sector(inode_table_lba, sector_buffer);
    ext2_inode_t root_test; dst = (uint8_t*)&root_test;
    for(size_t i=0; i<sizeof(ext2_inode_t); i++) dst[i] = sector_buffer[128 + i];
    if ((root_test.i_mode & 0xF000) != 0x4000) inode_size = 256;

    // 3. LEGGIAMO LA CARTELLA MADRE E CERCHIAMO DUPLICATI
    uint32_t p_index = parent_inode_num - 1;
    uint32_t p_sect_off = (p_index * inode_size) / 512;
    uint32_t p_in_sect_off = (p_index * inode_size) % 512;

    ata_read_sector(inode_table_lba + p_sect_off, sector_buffer);
    ext2_inode_t parent_inode;
    for(size_t i=0; i<sizeof(ext2_inode_t); i++) ((uint8_t*)&parent_inode)[i] = sector_buffer[p_in_sect_off + i];
    
    if ((parent_inode.i_mode & 0xF000) != 0x4000) return -2; // Non è una cartella

    uint32_t dir_block_lba = parent_inode.i_block[0] * 2;
    ata_read_sector(dir_block_lba, dir_buffer);
    ata_read_sector(dir_block_lba + 1, dir_buffer + 512);

    uint32_t offset = 0;
    uint32_t insert_offset = 0;
    uint32_t real_len_to_shrink = 0;

    // Analizziamo la cartella madre per trovare spazio vuoto e assicurarci che il nome sia libero
    while (offset < 1024) {
        ext2_dir_entry_t entry;
        for(size_t i=0; i<sizeof(ext2_dir_entry_t); i++) ((uint8_t*)&entry)[i] = dir_buffer[offset + i];
        if (entry.inode == 0 || entry.rec_len == 0) break;

        char fname[256];
        for(int k=0; k<entry.name_len; k++) fname[k] = dir_buffer[offset + 8 + k];
        fname[entry.name_len] = '\0';

        // Controllo Duplicato 
        int match = 1;
        if (entry.name_len != d_idx) match = 0;
        else {
            for(int k=0; k<d_idx; k++) { if (fname[k] != new_dirname[k]) { match = 0; break; } }
        }
        if (match) return -1; // IL NOME è GIà PRESO

        uint32_t real_len = 8 + entry.name_len;
        if (real_len % 4 != 0) real_len += 4 - (real_len % 4);

        if (entry.rec_len > real_len + 16) {
            insert_offset = offset;
            real_len_to_shrink = real_len;
        }
        offset += entry.rec_len;
    }

    if (insert_offset == 0 && offset > 0) return 0; // Nessuno spazio libero!


    // 4. CREAZIONE FISICA: ALLOCAZIONE
    uint32_t new_block = ext2_allocate_block();
    uint32_t new_inode = ext2_allocate_inode();
    if (new_block == 0 || new_inode == 0) return 0;

    // 5. SCRIVIAMO '.' e '..' NEL NUOVO BLOCCO
    for(int i=0; i<512; i++) {sector_buffer[i] = 0;}
    
    ext2_dir_entry_t dot;
    dot.inode = new_inode; dot.rec_len = 12; dot.name_len = 1; dot.file_type = 2;
    for(size_t i=0; i<sizeof(ext2_dir_entry_t); i++) sector_buffer[i] = ((uint8_t*)&dot)[i];
    sector_buffer[8] = '.';

    ext2_dir_entry_t dotdot;
    dotdot.inode = parent_inode_num; // Ora punta alla vera cartella madre
    dotdot.rec_len = 1012; dotdot.name_len = 2; dotdot.file_type = 2;
    for(size_t i=0; i<sizeof(ext2_dir_entry_t); i++) sector_buffer[12 + i] = ((uint8_t*)&dotdot)[i];
    sector_buffer[20] = '.'; sector_buffer[21] = '.';

    ata_write_sector(new_block * 2, sector_buffer);
    for(int i=0; i<512; i++) {sector_buffer[i] = 0;}
    ata_write_sector((new_block * 2) + 1, sector_buffer);

    // 6. CREIAMO LA CARTA D'IDENTITÀ (INODE) DELLA NUOVA CARTELLA
    uint32_t n_index = new_inode - 1;
    uint32_t n_sect_off = (n_index * inode_size) / 512;
    uint32_t n_in_sect_off = (n_index * inode_size) % 512;

    ata_read_sector(inode_table_lba + n_sect_off, sector_buffer);
    
    ext2_inode_t new_dir_inode;
    for(size_t i=0; i<sizeof(ext2_inode_t); i++) ((uint8_t*)&new_dir_inode)[i] = 0;
    new_dir_inode.i_mode = 0x41ED; 
    new_dir_inode.i_size = 1024;   
    new_dir_inode.i_links_count = 2; 
    new_dir_inode.i_blocks = 2; 
    new_dir_inode.i_block[0] = new_block; 

    for(size_t i=0; i<sizeof(ext2_inode_t); i++) sector_buffer[n_in_sect_off + i] = ((uint8_t*)&new_dir_inode)[i];
    ata_write_sector(inode_table_lba + n_sect_off, sector_buffer);

    // 7. AGGIUNGIAMO IL NOME ALLA CARTELLA MADRE
    ext2_dir_entry_t prev_entry;
    for(size_t i=0; i<sizeof(ext2_dir_entry_t); i++) ((uint8_t*)&prev_entry)[i] = dir_buffer[insert_offset + i];
    
    uint16_t old_rec_len = prev_entry.rec_len;
    prev_entry.rec_len = real_len_to_shrink;
    for(size_t i=0; i<sizeof(ext2_dir_entry_t); i++) dir_buffer[insert_offset + i] = ((uint8_t*)&prev_entry)[i];
    
    insert_offset += real_len_to_shrink;
    
    ext2_dir_entry_t new_entry;
    new_entry.inode = new_inode;
    new_entry.rec_len = old_rec_len - real_len_to_shrink;
    new_entry.name_len = d_idx;
    new_entry.file_type = 2; // 2 = DIRECTORY
    
    for(size_t i=0; i<sizeof(ext2_dir_entry_t); i++) dir_buffer[insert_offset + i] = ((uint8_t*)&new_entry)[i];
    for(int i=0; i<d_idx; i++) dir_buffer[insert_offset + 8 + i] = new_dirname[i]; 
    
    ata_write_sector(dir_block_lba, dir_buffer);

    return 1; 
}



// Legge un file dal disco usando il percorso assoluto e lo copia in memoria
int ext2_read_file(const char* full_path, char* dest_buffer, int max_size, uint32_t* out_file_size) {
    uint32_t target_inode_num = ext2_resolve_path(full_path);
    if (target_inode_num == 0) return 0; 
    
    ext2_inode_t file_inode = ext2_get_inode_struct(target_inode_num);
    if ((file_inode.i_mode & 0xF000) != 0x8000) return 0; // Deve essere un file
    
    uint32_t file_size = file_inode.i_size;
    if (out_file_size) *out_file_size = file_size; 

    if (max_size == 0) return 1; 

    if (file_size > (uint32_t)max_size) file_size = max_size;
    if (file_size == 0) return 1;

    // VFS: Usiamo il traduttore di blocchi per leggere senza intasare la RAM
    uint32_t total_blocks = (file_size + 1023) / 1024;
    uint8_t* stream_buffer = (uint8_t*)kmalloc(1024); // Solo 1 KB richiesto dall'Heap
    if (!stream_buffer) return 0;

    uint32_t bytes_loaded = 0;
    for (uint32_t logical_b = 0; logical_b < total_blocks; logical_b++) {
        uint32_t p_src = ext2_get_phys_block(&file_inode, logical_b);
        if (p_src == 0) break; // Corruzione file
        
        ata_read_sector(p_src * 2, stream_buffer);
        ata_read_sector((p_src * 2) + 1, stream_buffer + 512);
        
        for (int i = 0; i < 1024 && bytes_loaded < file_size; i++) {
            dest_buffer[bytes_loaded++] = stream_buffer[i];
        }
    }
    
    kfree(stream_buffer);
    if (bytes_loaded < (uint32_t)max_size) dest_buffer[bytes_loaded] = '\0'; 
    return 1; 
}

// Funzione di supporto per liberare in sicurezza un singolo blocco dati (Gestisce Bitmap enormi)
void ext2_free_block_safe(uint32_t block_num, uint32_t block_bitmap_lba) {
    if (block_num == 0) return;
    uint32_t bit_offset = block_num - 1;
    uint32_t sector_offset = (bit_offset / 8) / 512;
    uint32_t byte_in_sector = (bit_offset / 8) % 512;
    uint8_t bit_in_byte = bit_offset % 8;

    uint8_t bitmap_sec[512];
    if (ata_read_sector(block_bitmap_lba + sector_offset, bitmap_sec)) {
        bitmap_sec[byte_in_sector] &= ~(1 << bit_in_byte);
        ata_write_sector(block_bitmap_lba + sector_offset, bitmap_sec);
    }
}

// Elimina un file: nasconde il nome, libera i blocchi e ricicla l'Inode
int ext2_delete_file(const char* full_path) {
    // 1. SPLIT DEL PERCORSO: Separare cartella madre e nome file
    char parent_path[256];
    char filename[256];
    int last_slash = -1;
    int len = 0;
    
    while(full_path[len] != '\0' && len < 255) {
        if (full_path[len] == '/') last_slash = len;
        len++;
    }
    if (last_slash == -1 || len == 1) return 0; // Percorso non valido

    if (last_slash == 0) { 
        parent_path[0] = '/'; parent_path[1] = '\0';
    } else {
        for(int i=0; i<last_slash; i++) parent_path[i] = full_path[i];
        parent_path[last_slash] = '\0';
    }
    
    int f_idx = 0;
    for(int i=last_slash+1; i<len; i++) filename[f_idx++] = full_path[i];
    filename[f_idx] = '\0';
    if (f_idx == 0) return 0;

    // 2. RISOLUZIONE DELLA CARTELLA MADRE
    uint32_t parent_inode_num = ext2_resolve_path(parent_path);
    if (parent_inode_num == 0) return 0; // Errore: Cartella madre inesistente

    uint8_t sector_buffer[512] = {0};
    uint8_t dir_buffer[1024] = {0};

    // Leggiamo la BGDT per sapere dove sono le Mappe e le Tabelle
    if (!ata_read_sector(4, sector_buffer)) return 0;
    ext2_bg_desc_t bgd; uint8_t* dst = (uint8_t*)&bgd;
    for(size_t i=0; i<sizeof(ext2_bg_desc_t); i++) dst[i] = sector_buffer[i];
    
    uint32_t inode_table_lba = bgd.bg_inode_table * 2;
    uint32_t block_bitmap_lba = bgd.bg_block_bitmap * 2;
    uint32_t inode_bitmap_lba = bgd.bg_inode_bitmap * 2;

    uint32_t inode_size = 128;
    ata_read_sector(inode_table_lba, sector_buffer);
    ext2_inode_t root_test; dst = (uint8_t*)&root_test;
    for(size_t i=0; i<sizeof(ext2_inode_t); i++) dst[i] = sector_buffer[128 + i];
    if ((root_test.i_mode & 0xF000) != 0x4000) inode_size = 256;

    // 3. APRIAMO LA CARTELLA MADRE E CERCHIAMO IL FILE
    uint32_t p_index = parent_inode_num - 1;
    uint32_t p_sect_off = (p_index * inode_size) / 512;
    uint32_t p_in_sect_off = (p_index * inode_size) % 512;

    if (!ata_read_sector(inode_table_lba + p_sect_off, sector_buffer)) return 0;
    
    ext2_inode_t parent_inode;
    for(size_t i=0; i<sizeof(ext2_inode_t); i++) ((uint8_t*)&parent_inode)[i] = sector_buffer[p_in_sect_off + i];
    
    if ((parent_inode.i_mode & 0xF000) != 0x4000) return 0; // Non è una cartella

    uint32_t dir_block_lba = parent_inode.i_block[0] * 2;
    if (!ata_read_sector(dir_block_lba, dir_buffer)) return 0;

    uint32_t offset = 0;
    uint32_t prev_offset = 0; 
    uint32_t target_inode_num = 0;
    uint32_t entry_rec_len = 0;

    while (offset < 1024) {
        ext2_dir_entry_t entry;
        for(size_t i=0; i<sizeof(ext2_dir_entry_t); i++) ((uint8_t*)&entry)[i] = dir_buffer[offset + i];
        
        // SICUREZZA EXTRA: Usciamo subito se la chain è corrotta
        if (entry.inode == 0 || entry.rec_len == 0) break;

        char fname[256];
        for (int k = 0; k < entry.name_len; k++) fname[k] = dir_buffer[offset + 8 + k];
        fname[entry.name_len] = '\0';
        
        int match = 1;
        if (entry.name_len != f_idx) match = 0;
        else {
            for(int k=0; k<f_idx; k++) { if (fname[k] != filename[k]) { match = 0; break; } }
        }

        if (match) {
            target_inode_num = entry.inode;
            entry_rec_len = entry.rec_len;
            break; 
        }
        prev_offset = offset;
        offset += entry.rec_len;
    }
    
    // SE IL FILE NON ESISTE, IL DISCO NON VIENE TOCCATO
    if (target_inode_num == 0) return 0; 

    // 4. LEGGERE L'INODE DEL FILE E VERIFICARE I PERMESSI
    uint32_t index = target_inode_num - 1;
    uint32_t sect_off = (index * inode_size) / 512;
    uint32_t in_sect_off = (index * inode_size) % 512;
    
    uint8_t temp_buffer[512]; 
    if (!ata_read_sector(inode_table_lba + sect_off, temp_buffer)) return 0;
    
    ext2_inode_t target_inode;
    for(size_t i=0; i<sizeof(ext2_inode_t); i++) ((uint8_t*)&target_inode)[i] = temp_buffer[in_sect_off + i];

    if ((target_inode.i_mode & 0xF000) == 0x4000) {
        return -1; // Sicurezza: è una directory, blocca tutto
    }

    // 5. Prevenzione corruzione
    // A. Azzeriamo l'Inode dell'entry bersaglio (Questo dice a e2fsck/e2cp che il file è davvero morto)
    ext2_dir_entry_t target_entry;
    for(size_t i=0; i<sizeof(ext2_dir_entry_t); i++) ((uint8_t*)&target_entry)[i] = dir_buffer[offset + i];
    target_entry.inode = 0; // IL COLPO DI GRAZIA
    for(size_t i=0; i<sizeof(ext2_dir_entry_t); i++) dir_buffer[offset + i] = ((uint8_t*)&target_entry)[i];

    // B. Estendiamo l'entry precedente per fargli inghiottire lo spazio
    if (prev_offset != offset) { 
        ext2_dir_entry_t prev_entry;
        for(size_t i=0; i<sizeof(ext2_dir_entry_t); i++) ((uint8_t*)&prev_entry)[i] = dir_buffer[prev_offset + i];
        
        prev_entry.rec_len += entry_rec_len; 
        
        for(size_t i=0; i<sizeof(ext2_dir_entry_t); i++) dir_buffer[prev_offset + i] = ((uint8_t*)&prev_entry)[i];
    }
    
    // Unico punto in cui modifichiamo la cartella sul disco
    ata_write_sector(dir_block_lba, dir_buffer);
    
    
    // ===============================================
    // LIBERIAMO I BLOCCHI (Diretti, Indiretti e Doppi) E L'INODE NELLA BITMAP
    // ===============================================
    // 1. Blocchi Diretti (0-11)
    for (int b = 0; b < 12; b++) ext2_free_block_safe(target_inode.i_block[b], block_bitmap_lba);

    

    // 2. Singolo Indiretto (Blocco 12)
    if (target_inode.i_block[12] != 0) {
        uint32_t* ind_ptrs = (uint32_t*)kmalloc(1024);
        if (ind_ptrs) {
            ata_read_sector(target_inode.i_block[12] * 2, (uint8_t*)ind_ptrs);
            ata_read_sector((target_inode.i_block[12] * 2) + 1, ((uint8_t*)ind_ptrs) + 512);
            for (int ib = 0; ib < 256; ib++) {ext2_free_block_safe(ind_ptrs[ib], block_bitmap_lba);}
            kfree(ind_ptrs);
        }
        ext2_free_block_safe(target_inode.i_block[12], block_bitmap_lba); 
    }

    // 3. Doppio Indiretto (Blocco 13)
    if (target_inode.i_block[13] != 0) {
        uint32_t* mast_ptrs = (uint32_t*)kmalloc(1024);
        uint32_t* sub_ptrs = (uint32_t*)kmalloc(1024);
        if (mast_ptrs && sub_ptrs) {
            ata_read_sector(target_inode.i_block[13] * 2, (uint8_t*)mast_ptrs);
            ata_read_sector((target_inode.i_block[13] * 2) + 1, ((uint8_t*)mast_ptrs) + 512);
            for (int m = 0; m < 256; m++) {
                if (mast_ptrs[m] != 0) {
                    ata_read_sector(mast_ptrs[m] * 2, (uint8_t*)sub_ptrs);
                    ata_read_sector((mast_ptrs[m] * 2) + 1, ((uint8_t*)sub_ptrs) + 512);
                    for (int s = 0; s < 256; s++) {ext2_free_block_safe(sub_ptrs[s], block_bitmap_lba);}
                    ext2_free_block_safe(mast_ptrs[m], block_bitmap_lba);
                }
            }
        }
        if(mast_ptrs) kfree(mast_ptrs);
        if(sub_ptrs) kfree(sub_ptrs);
        ext2_free_block_safe(target_inode.i_block[13], block_bitmap_lba);
    }



    // 4. Infine, liberiamo l'Inode stesso
    uint8_t bitmap_buffer[512];
    if (ata_read_sector(inode_bitmap_lba, bitmap_buffer)) {
        uint32_t byte_idx = (target_inode_num - 1) / 8;
        uint8_t bit = (target_inode_num - 1) % 8;
        bitmap_buffer[byte_idx] &= ~(1 << bit);
        ata_write_sector(inode_bitmap_lba, bitmap_buffer);
    }
    

    return 1; 
}


// ==============================================================================
// MOTORE DI DISTRUZIONE RICORSIVA EXT2 (Non reversibile)
// ==============================================================================

// Funzione interna che vaporizza un Inode e tutto ciò che contiene
void ext2_wipe_inode(uint32_t target_inode_num) {
    if (target_inode_num == 0) return;

    // Usiamo kmalloc per non intasare lo Stack del Kernel durante la ricorsione
    uint8_t* sector_buffer = (uint8_t*)kmalloc(512);
    if (!sector_buffer) return;

    if (!ata_read_sector(4, sector_buffer)) { kfree(sector_buffer); return; }
    ext2_bg_desc_t bgd; uint8_t* dst = (uint8_t*)&bgd;
    for(size_t i=0; i<sizeof(ext2_bg_desc_t); i++) dst[i] = sector_buffer[i];

    uint32_t inode_table_lba = bgd.bg_inode_table * 2;
    uint32_t block_bitmap_lba = bgd.bg_block_bitmap * 2;
    uint32_t inode_bitmap_lba = bgd.bg_inode_bitmap * 2;

    uint32_t inode_size = 128;
    ata_read_sector(inode_table_lba, sector_buffer);
    ext2_inode_t root_test; dst = (uint8_t*)&root_test;
    for(size_t i=0; i<sizeof(ext2_inode_t); i++) dst[i] = sector_buffer[128 + i];
    if ((root_test.i_mode & 0xF000) != 0x4000) inode_size = 256;

    uint32_t index = target_inode_num - 1;
    uint32_t sect_off = (index * inode_size) / 512;
    uint32_t in_sect_off = (index * inode_size) % 512;

    if (!ata_read_sector(inode_table_lba + sect_off, sector_buffer)) { kfree(sector_buffer); return; }
    ext2_inode_t target_inode;
    for(size_t i=0; i<sizeof(ext2_inode_t); i++) ((uint8_t*)&target_inode)[i] = sector_buffer[in_sect_off + i];

    // ===============================================
    // SE E' UNA CARTELLA, LA SVUOTIAMO RICORSIVAMENTE
    // ===============================================
    if ((target_inode.i_mode & 0xF000) == 0x4000) {
        uint8_t* dir_buffer = (uint8_t*)kmalloc(1024); // Raddoppiato
        if (dir_buffer) {
            for (int b = 0; b < 12; b++) {
                if (target_inode.i_block[b] == 0) break;
                uint32_t dir_lba = target_inode.i_block[b] * 2;
                
                // Leggiamo entrambi i settori del blocco in un colpo solo
                if (ata_read_sector(dir_lba, dir_buffer) && ata_read_sector(dir_lba + 1, dir_buffer + 512)) {
                    uint32_t offset = 0;
                    while (offset < 1024) {
                        ext2_dir_entry_t entry;
                        for(size_t i=0; i<sizeof(ext2_dir_entry_t); i++) ((uint8_t*)&entry)[i] = dir_buffer[offset + i];
                        if (entry.inode == 0 || entry.rec_len == 0) break;

                        char fname[256];
                        for (int k = 0; k < entry.name_len; k++) fname[k] = dir_buffer[offset + 8 + k];
                        fname[entry.name_len] = '\0';

                        int is_dot = (entry.name_len == 1 && fname[0] == '.');
                        int is_dotdot = (entry.name_len == 2 && fname[0] == '.' && fname[1] == '.');

                        if (!is_dot && !is_dotdot) {
                            ext2_wipe_inode(entry.inode);
                        }
                        offset += entry.rec_len;
                    }
                }
            }
            kfree(dir_buffer);
        }
    }

    // ===============================================
    // LIBERIAMO I BLOCCHI (Diretti, Indiretti e Doppi) E L'INODE NELLA BITMAP
    // ===============================================
    // 1. Blocchi Diretti (0-11)
    for (int b = 0; b < 12; b++) ext2_free_block_safe(target_inode.i_block[b], block_bitmap_lba);

    // 2. Singolo Indiretto (Blocco 12)
    if (target_inode.i_block[12] != 0) {
        uint32_t* ind_ptrs = (uint32_t*)kmalloc(1024);
        if (ind_ptrs) {
            ata_read_sector(target_inode.i_block[12] * 2, (uint8_t*)ind_ptrs);
            ata_read_sector((target_inode.i_block[12] * 2) + 1, ((uint8_t*)ind_ptrs) + 512);
            for (int ib = 0; ib < 256; ib++) {ext2_free_block_safe(ind_ptrs[ib], block_bitmap_lba);}
            kfree(ind_ptrs);
        }
        ext2_free_block_safe(target_inode.i_block[12], block_bitmap_lba); 
    }

    // 3. Doppio Indiretto (Blocco 13)
    if (target_inode.i_block[13] != 0) {
        uint32_t* mast_ptrs = (uint32_t*)kmalloc(1024);
        uint32_t* sub_ptrs = (uint32_t*)kmalloc(1024);
        if (mast_ptrs && sub_ptrs) {
            ata_read_sector(target_inode.i_block[13] * 2, (uint8_t*)mast_ptrs);
            ata_read_sector((target_inode.i_block[13] * 2) + 1, ((uint8_t*)mast_ptrs) + 512);
            for (int m = 0; m < 256; m++) {
                if (mast_ptrs[m] != 0) {
                    ata_read_sector(mast_ptrs[m] * 2, (uint8_t*)sub_ptrs);
                    ata_read_sector((mast_ptrs[m] * 2) + 1, ((uint8_t*)sub_ptrs) + 512);
                    for (int s = 0; s < 256; s++) {ext2_free_block_safe(sub_ptrs[s], block_bitmap_lba);}
                    ext2_free_block_safe(mast_ptrs[m], block_bitmap_lba);
                }
            }
        }
        if(mast_ptrs) kfree(mast_ptrs);
        if(sub_ptrs) kfree(sub_ptrs);
        ext2_free_block_safe(target_inode.i_block[13], block_bitmap_lba);
    }

    // 4. Infine, liberiamo l'Inode stesso
    uint8_t bitmap_buffer[512];
    if (ata_read_sector(inode_bitmap_lba, bitmap_buffer)) {
        uint32_t byte_idx = (target_inode_num - 1) / 8;
        uint8_t bit = (target_inode_num - 1) % 8;
        bitmap_buffer[byte_idx] &= ~(1 << bit);
        ata_write_sector(inode_bitmap_lba, bitmap_buffer);
    }

    kfree(sector_buffer); // Pulizia RAM Kerne
}

// L'API Pubblica: Rimuove la cartella dall'albero Ext2 e scatena l'annientamento
int ext2_remove_directory(const char* full_path) {
    char parent_path[256];
    char dirname[256];
    int last_slash = -1;
    int len = 0;

    while(full_path[len] != '\0' && len < 255) {
        if (full_path[len] == '/') last_slash = len;
        len++;
    }
    if (last_slash == -1 || len <= 1) return 0; // Previene l'eliminazione della Root (/)

    if (last_slash == 0) {
        parent_path[0] = '/'; parent_path[1] = '\0';
    } else {
        for(int i=0; i<last_slash; i++) parent_path[i] = full_path[i];
        parent_path[last_slash] = '\0';
    }

    int d_idx = 0;
    for(int i=last_slash+1; i<len; i++) dirname[d_idx++] = full_path[i];
    dirname[d_idx] = '\0';
    if (d_idx == 0) return 0;

    uint32_t parent_inode_num = ext2_resolve_path(parent_path);
    if (parent_inode_num == 0) return 0;

    uint8_t sector_buffer[512] = {0};
    uint8_t dir_buffer[1024] = {0};

    if (!ata_read_sector(4, sector_buffer)) return 0;
    ext2_bg_desc_t bgd; uint8_t* dst = (uint8_t*)&bgd;
    for(size_t i=0; i<sizeof(ext2_bg_desc_t); i++) dst[i] = sector_buffer[i];

    uint32_t inode_table_lba = bgd.bg_inode_table * 2;
    uint32_t inode_size = 128;
    ata_read_sector(inode_table_lba, sector_buffer);
    ext2_inode_t root_test; dst = (uint8_t*)&root_test;
    for(size_t i=0; i<sizeof(ext2_inode_t); i++) dst[i] = sector_buffer[128 + i];
    if ((root_test.i_mode & 0xF000) != 0x4000) inode_size = 256;

    uint32_t p_index = parent_inode_num - 1;
    uint32_t p_sect_off = (p_index * inode_size) / 512;
    uint32_t p_in_sect_off = (p_index * inode_size) % 512;

    if (!ata_read_sector(inode_table_lba + p_sect_off, sector_buffer)) return 0;
    ext2_inode_t parent_inode;
    for(size_t i=0; i<sizeof(ext2_inode_t); i++) ((uint8_t*)&parent_inode)[i] = sector_buffer[p_in_sect_off + i];
    if ((parent_inode.i_mode & 0xF000) != 0x4000) return 0;

    uint32_t dir_block_lba = parent_inode.i_block[0] * 2;
    if (!ata_read_sector(dir_block_lba, dir_buffer)) return 0;

    uint32_t offset = 0;
    uint32_t prev_offset = 0;
    uint32_t target_inode_num = 0;
    uint32_t entry_rec_len = 0;

    while (offset < 1024) {
        ext2_dir_entry_t entry;
        for(size_t i=0; i<sizeof(ext2_dir_entry_t); i++) ((uint8_t*)&entry)[i] = dir_buffer[offset + i];
        if (entry.inode == 0 || entry.rec_len == 0) break;

        char fname[256];
        for (int k = 0; k < entry.name_len; k++) fname[k] = dir_buffer[offset + 8 + k];
        fname[entry.name_len] = '\0';

        int match = 1;
        if (entry.name_len != d_idx) match = 0;
        else {
            for(int k=0; k<d_idx; k++) { if (fname[k] != dirname[k]) { match = 0; break; } }
        }

        if (match) {
            target_inode_num = entry.inode;
            entry_rec_len = entry.rec_len;
            break;
        }
        prev_offset = offset;
        offset += entry.rec_len;
    }

    if (target_inode_num == 0) return 0; 

    // VERIFICA FINALE: Controlliamo che sia DAVVERO una cartella prima di distruggere
    uint32_t index = target_inode_num - 1;
    uint32_t sect_off = (index * inode_size) / 512;
    uint32_t in_sect_off = (index * inode_size) % 512;

    uint8_t temp_buffer[512];
    if (!ata_read_sector(inode_table_lba + sect_off, temp_buffer)) return 0;

    ext2_inode_t target_inode;
    for(size_t i=0; i<sizeof(ext2_inode_t); i++) ((uint8_t*)&target_inode)[i] = temp_buffer[in_sect_off + i];

    if ((target_inode.i_mode & 0xF000) != 0x4000) {
        return -1; // ERRORE: L'utente sta provando a usare rmdir su un file (.txt, .edxi)
    }

    // SCUCIAMO IL NOME DALLA CARTELLA MADRE E AZZERIAMO L'INODE
    ext2_dir_entry_t target_entry;
    for(size_t i=0; i<sizeof(ext2_dir_entry_t); i++) ((uint8_t*)&target_entry)[i] = dir_buffer[offset + i];
    target_entry.inode = 0; // IL COLPO DI GRAZIA
    for(size_t i=0; i<sizeof(ext2_dir_entry_t); i++) dir_buffer[offset + i] = ((uint8_t*)&target_entry)[i];

    if (prev_offset != offset) {
        ext2_dir_entry_t prev_entry;
        for(size_t i=0; i<sizeof(ext2_dir_entry_t); i++) ((uint8_t*)&prev_entry)[i] = dir_buffer[prev_offset + i];
        prev_entry.rec_len += entry_rec_len;
        for(size_t i=0; i<sizeof(ext2_dir_entry_t); i++) dir_buffer[prev_offset + i] = ((uint8_t*)&prev_entry)[i];
    }
    ata_write_sector(dir_block_lba, dir_buffer);

    // INNESCHIAMO LA BOMBA RICORSIVA
    ext2_wipe_inode(target_inode_num);

    return 1;
}



// Motore di Copia VFS a Flusso Continuo (Consuma solo 1KB di RAM, zero limiti di file)
int ext2_copy_file(const char* src_path, const char* dest_path) {
    // 0. CONTROLLO LUCCHETTO
    if (vfs_lock == 1) return -6; // Codice Errore: DISCO OCCUPATO
    
    uint32_t src_size = 0;
    
    // 1. Otteniamo la grandezza senza caricare il file
    if (!ext2_read_file(src_path, NULL, 0, &src_size)) return -3;
    if (src_size == 0) return -4; // File vuoto, nulla da copiare
    
    // 2. Risolviamo l'Inode sorgente
    uint32_t src_inode_num = ext2_resolve_path(src_path);
    if (!src_inode_num) return -3;


    // DA QUI IN POI, NESSUN ALTRO PUÒ TOCCARE IL DISCO
    vfs_lock = 1;

    // 3. PRE-ALLOCAZIONE! Creiamo il file di destinazione vuoto (pieno di zeri)
    int alloc_res = ext2_write_file(dest_path, NULL, src_size);
    if (alloc_res != 1) { vfs_lock = 0; return alloc_res; } // Sblocchiamo in caso di errore
    
    uint32_t dest_inode_num = ext2_resolve_path(dest_path);

    // 4. Carichiamo in RAM le carte d'identità dei due file
    ext2_inode_t src_inode = ext2_get_inode_struct(src_inode_num);
    ext2_inode_t dest_inode = ext2_get_inode_struct(dest_inode_num);

    // 5. Setup Grafica
    if (src_size > 131072) {
        show_progress_bar = 1;
        progress_total = src_size * 2; 
        progress_current = 0;
    } else show_progress_bar = 0;

    // =======================================================
    // IL MOTORE VFS (STREAMING CHUNKING)
    // =======================================================
    uint32_t total_blocks = (src_size + 1023) / 1024;
    uint8_t* stream_buffer = (uint8_t*)kmalloc(1024); // SOLO 1 KB DI RAM
    if (!stream_buffer) { vfs_lock = 0; return -5; } // Sblocchiamo in caso di errore OOM

    // Copiamo fisicamente 1 Kilobyte alla volta, seguendo il labirinto dei puntatori
    for (uint32_t logical_b = 0; logical_b < total_blocks; logical_b++) {
        
        // La Pietra di Rosetta ci rivela l'indirizzo fisico reale
        uint32_t p_src = ext2_get_phys_block(&src_inode, logical_b);
        uint32_t p_dest = ext2_get_phys_block(&dest_inode, logical_b);

        if (p_src == 0 || p_dest == 0) break; // Sicurezza corruzione

        // Legge 1024 byte dalla sorgente
        ata_read_sector(p_src * 2, stream_buffer);
        ata_read_sector((p_src * 2) + 1, stream_buffer + 512);

        // Scrive 1024 byte nella destinazione
        ata_write_sector(p_dest * 2, stream_buffer);
        ata_write_sector((p_dest * 2) + 1, stream_buffer + 512);
    }
    
    kfree(stream_buffer);
    show_progress_bar = 0;

    // OPERAZIONE FINITA: TOGLIAMO IL LUCCHETTO
    vfs_lock = 0;
    return 1; 
}


// ==========================================
// LOADABLE KERNEL MODULES (LKM) LOADER
// ==========================================
int load_driver(const char* path) {
    uint32_t file_size = 0;
    
    // 1. Chiediamo al file system quanto è grande il file del driver
    if (!ext2_read_file(path, NULL, 0, &file_size)) return -1; // File inesistente
    if (file_size == 0) return -2; // File vuoto

    // 2. Allochiamo la RAM direttamente nel Kernel Heap (Ring 0, Nessun limite di Page Table)
    void* driver_mem = kmalloc(file_size);
    if (!driver_mem) return -3; // OOM (Memoria Esaurita)

    // 3. Copiamo fisicamente il codice macchina dal disco alla RAM
    if (!ext2_read_file(path, (char*)driver_mem, file_size, &file_size)) {
        kfree(driver_mem);
        return -4; // Errore di lettura disco
    }

    // 4. LA MAGIA: Passiamo al driver la SUA STESSA posizione in memoria
    int (*driver_entry)(kernel_api_t*, void*) = (int (*)(kernel_api_t*, void*))driver_mem;

    // 5. Inneschiamo il Driver passandogli la "driver_api" e la sua Base (driver_mem)
    int result = driver_entry(&driver_api, driver_mem);
    
    // Se il driver restituisce 0 (Fallimento) o se non deve restare in memoria residente
    // per adesso liberiamo la memoria. In futuro i driver audio resteranno attivi!
    
    
    return result; 
}


// ==========================================
// GESTIONE DELLA MEMORIA (HEAP)
// ==========================================
// Decidiamo di far partire il nostro "Magazzino" dal Mega numero 16 della RAM
// (Un posto sicuro e lontano dal Kernel e dalla scheda video)
#define HEAP_START 0x01000000 
#define HEAP_SIZE  0x01000000 // 16 Megabyte di spazio a disposizione


// ==========================================
// PHYSICAL FRAME ALLOCATOR (Dinamico)
// ==========================================
uint32_t system_ram_mb = 0;
uint32_t pfa_start_addr = 0x02000000; // La RAM delle app inizia sempre dopo i primi 32 MB
uint32_t pfa_max_frames = 0;
uint32_t pfa_total_memory = 0;

// Supportiamo fino a 1 GigaByte di RAM fisica (262144 frame / 32 = 8192 interi)
uint32_t pfa_bitmap[8192]; 

void init_pfa(uint32_t total_ram_mb) {
    system_ram_mb = total_ram_mb;
    
    // Cap di sicurezza a 1024 MB (1 GigaByte) per non far esplodere l'array
    if (system_ram_mb > 1024) system_ram_mb = 1024;
    
    // Il Kernel, la VRAM e l'Heap usano i primi 32 MB. Il resto è per le App.
    if (system_ram_mb <= 32) {
        pfa_max_frames = 0; pfa_total_memory = 0;
    } else {
        pfa_total_memory = (system_ram_mb - 32) * 1024 * 1024;
        pfa_max_frames = pfa_total_memory / 4096;
    }

    // Inizializziamo l'array mappando solo la RAM esistente
    for (uint32_t i = 0; i < 8192; i++) {
        if (i < (pfa_max_frames / 32)) {
            pfa_bitmap[i] = 0; // Frame fisicamente esistente e libero
        } else {
            pfa_bitmap[i] = 0xFFFFFFFF; // RAM inesistente, bloccata in eterno
        }
    }
}

uint32_t pfa_alloc_frame() {
    if (pfa_max_frames == 0) return 0;
    
    for (uint32_t i = 0; i < (pfa_max_frames / 32) + 1; i++) {
        if (pfa_bitmap[i] != 0xFFFFFFFF) { 
            for (int bit = 0; bit < 32; bit++) {
                if (!(pfa_bitmap[i] & (1 << bit))) {
                    pfa_bitmap[i] |= (1 << bit); // Lo marchiamo occupato
                    uint32_t frame_index = (i * 32) + bit;
                    if (frame_index >= pfa_max_frames) return 0; // Sicurezza fine RAM
                    return pfa_start_addr + (frame_index * 4096);
                }
            }
        }
    }
    return 0; 
}

void pfa_free_frame(uint32_t physical_address) {
    if (physical_address < pfa_start_addr || physical_address >= pfa_start_addr + pfa_total_memory) return;
    uint32_t frame_index = (physical_address - pfa_start_addr) / 4096;
    uint32_t array_index = frame_index / 32;
    uint32_t bit_index = frame_index % 32;
    pfa_bitmap[array_index] &= ~(1 << bit_index); // Lo liberiamo
}



// L'intestazione segreta di ogni blocco (Con Padding)
typedef struct mem_block {
    size_t size;            
    int is_free;            
    struct mem_block* next; 
    // Riempiamo lo spazio residuo per far pesare questa struct esattamente 4096 byte
    uint8_t padding[4096 - 12]; 
} mem_block_t;


// Il "Capo" della lista
mem_block_t* heap_head = (mem_block_t*)HEAP_START;

// Inizializza la RAM (Da chiamare all'avvio)
void init_heap() {
    heap_head->size = HEAP_SIZE - sizeof(mem_block_t);
    heap_head->is_free = 1;
    heap_head->next = NULL;
}


// Alloca la memoria dinamica
void* kmalloc(size_t size) {
    // Arrotondiamo la dimensione richiesta al multiplo di 4KB più vicino
    if (size % 4096 != 0) size = size + (4096 - (size % 4096));

    mem_block_t* current = heap_head;
    while (current != NULL) {
        if (current->is_free && current->size >= size) {
            if (current->size > size + sizeof(mem_block_t) + 16) {
                mem_block_t* new_block = (mem_block_t*)((uint8_t*)current + sizeof(mem_block_t) + size);
                new_block->size = current->size - size - sizeof(mem_block_t);
                new_block->is_free = 1;
                new_block->next = current->next;
                current->size = size;
                current->next = new_block;
            }
            current->is_free = 0;
            return (void*)((uint8_t*)current + sizeof(mem_block_t));
        }
        current = current->next;
    }
    return NULL; 
}

// Libera la memoria
void kfree(void* ptr) {
    if (!ptr) return;
    
    // Facciamo un passo indietro per leggere l'intestazione segreta
    mem_block_t* block = (mem_block_t*)((uint8_t*)ptr - sizeof(mem_block_t));
    block->is_free = 1;
    
    // De-frammentazione: Se due blocchi vicini sono liberi, li fondiamo in uno solo
    mem_block_t* current = heap_head;
    while (current != NULL) {
        if (current->is_free && current->next != NULL && current->next->is_free) {
            current->size += sizeof(mem_block_t) + current->next->size;
            current->next = current->next->next;
        } else {
            current = current->next;
        }
    }
}


// Il chip DMA antico non vede oltre 16MB: restiamo sotto quella soglia,
// ma SOPRA la fine del kernel (backbuffer + finestre lo portano oltre i 4MB).
#define DMA_HEAP_MIN   0x00800000   // 8 MB: pavimento di sicurezza
#define DMA_HEAP_SIZE  (2 * 1024 * 1024)

extern char kernel_end[];   // simbolo fornito dal linker

mem_block_t* dma_heap_head = 0;   // l'indirizzo vero lo calcola init_dma_heap()

void init_dma_heap() {
    // Partiamo dopo la fine del kernel, arrotondata al megabyte,
    // e comunque mai sotto il pavimento di sicurezza.
    uint32_t base = ((uint32_t)kernel_end + 0xFFFFF) & ~0xFFFFFu;
    if (base < DMA_HEAP_MIN) base = DMA_HEAP_MIN;

    dma_heap_head = (mem_block_t*)base;
    dma_heap_head->size = DMA_HEAP_SIZE - sizeof(mem_block_t);
    dma_heap_head->is_free = 1;
    dma_heap_head->next = NULL;
}

// Identico a kmalloc, ma pesca dal magazzino basso
void* kmalloc_dma(size_t size) {
    if (size % 4096 != 0) size = size + (4096 - (size % 4096));

    mem_block_t* current = dma_heap_head;
    while (current != NULL) {
        if (current->is_free && current->size >= size) {
            // FONDAMENTALE PER IL DMA: Il blocco non deve attraversare un confine di 64KB
            // Se lo fa, scartiamo questo blocco e passiamo al prossimo.
            uint32_t start_addr = (uint32_t)current + sizeof(mem_block_t);
            uint32_t end_addr = start_addr + size - 1;
            // Modifica il divisore per il DMA a 16-bit
            if ((start_addr / 131072) != (end_addr / 131072)) {
                current = current->next;
                continue;
            }

            if (current->size > size + sizeof(mem_block_t) + 16) {
                mem_block_t* new_block = (mem_block_t*)((uint8_t*)current + sizeof(mem_block_t) + size);
                new_block->size = current->size - size - sizeof(mem_block_t);
                new_block->is_free = 1;
                new_block->next = current->next;
                current->size = size;
                current->next = new_block;
            }
            current->is_free = 0;
            return (void*)start_addr;
        }
        current = current->next;
    }
    return NULL; 
}

// Libera la memoria nel magazzino basso (DMA)
void kfree_dma(void* ptr) {
    if (!ptr) return;
    
    // Leggiamo l'intestazione segreta
    mem_block_t* block = (mem_block_t*)((uint8_t*)ptr - sizeof(mem_block_t));
    block->is_free = 1;
    
    // De-frammentazione del DMA Heap
    mem_block_t* current = dma_heap_head;
    while (current != NULL) {
        if (current->is_free && current->next != NULL && current->next->is_free) {
            current->size += sizeof(mem_block_t) + current->next->size;
            current->next = current->next->next;
        } else {
            current = current->next;
        }
    }
}


// ============================================================================
// CHIUSURA DI UN'APPLICAZIONE
// Un'app si chiude in quattro modi: il pulsante X della sua finestra, exit()
// chiamata dall'app stessa, un crash, il comando kill del terminale. Tutti e
// quattro passano da qui: ogni effetto della chiusura si scrive una volta
// sola, in questa funzione, e vale per tutte le strade.
//
// Nell'ordine:
//   1. chiude tutte le finestre del processo, la madre e le figlie
//   2. libera la sua memoria: stack, pagine fisiche, tabella delle pagine
//   3. rimette a posto la taskbar, che poteva essere schiacciata sotto una
//      sua finestra massimizzata
//
// Non ferma la CPU. Se il processo che si chiude e' quello in esecuzione
// (exit o crash), chi chiama deve poi usare app_wait_for_end(): quel codice
// non esiste piu', e non c'e' nulla a cui tornare.
// ============================================================================
static void close_app(int pid) {
    if (pid <= 0 || pid >= MAX_PROCESSES || !process_table[pid].active) return;

    // 1. Le finestre
    for (int w = 0; w < MAX_WINDOWS; w++) {
        if (windows[w].active && windows[w].owner_pid == pid) {
            windows[w].active = 0;
        }
    }

    // 2. La memoria. Il processo si segna come spento per primo: da questo
    //    momento lo scheduler non lo scegliera' piu'.
    process_table[pid].active = 0;
    kfree((void*)process_table[pid].stack_base);

    uint32_t* pt = process_table[pid].page_table;
    if (pt) {
        for (int i = 0; i < 1024; i++) {
            if (pt[i] & 0x01) {   // Pagina presente: restituiamo il frame fisico
                pfa_free_frame(pt[i] & 0xFFFFF000);
            }
        }
        kfree(pt);
    }

    // 3. La taskbar
    update_taskbar_state();
    refresh_requested = 1;
}

// Il processo in esecuzione si e' appena chiuso: si aspetta che lo scheduler
// passi ad altro, e non si torna mai piu' qui.
static void app_wait_for_end(void) {
    // IL SALVAVITA: riaccendiamo il timer, altrimenti lo scheduler non
    // arriverebbe mai a toglierci la CPU
    asm volatile("sti");
    while (1) { asm volatile("hlt"); }
}

void kill_process() {
    close_app(current_pid);
    app_wait_for_end();
}


// Helper: Trova il primo PID libero
int get_free_pid() {
    for(int i=1; i<MAX_PROCESSES; i++) {
        if(!process_table[i].active) return i;
    }
    return -1;
}

int create_process(void (*entry_point)(), int window_id, uint32_t* private_pt, int is_virtual, int parent_window_id, int pid) {
    if(pid <= 0 || pid >= MAX_PROCESSES) return -1;

    uint32_t* stack = (uint32_t*)kmalloc(4096); // 4KB di Stack privato
    process_table[pid].stack_base = (uint32_t)stack;
    process_table[pid].page_table = private_pt; // SALVIAMO LA PAGE TABLE
    process_table[pid].linked_window = window_id;
    process_table[pid].parent_window = parent_window_id;
    process_table[pid].is_critical_io = 0; // INIZIALIZZAZIONE DELLO SCUDO

    // Andiamo in cima alla RAM privata e scendiamo al contrario
    uint32_t* st = (uint32_t*)((uint8_t*)stack + 4096);

    // 1. Se l'app esegue 'ret', salterà qui per suicidarsi pulendo la RAM
    *(--st) = (uint32_t)kill_process;

    
    
    // 2. L'Hardware Frame: i valori esatti che il comando 'iret' si aspetta di trovare
    if (is_virtual) {
        // === LANCIO IN RING 3 (USER MODE) ===
        // Per andare in Ring 3, IRET pretende 5 valori
        *(--st) = 0x23;                 // SS  (Segmento Dati Utente: 0x20 + Ring 3)
        *(--st) = 0x40020000;           // ESP (User Stack: 128KB dopo l'inizio. Spazio per le big apps!)
        *(--st) = 0x0202;               // EFLAGS (Interrupt Abilitati)
        *(--st) = 0x1B;                 // CS  (Segmento Codice Utente: 0x18 + Ring 3)
        *(--st) = (uint32_t)0x40000010; // EIP (Punta dopo l'header EDXI)
    } else {
        // === LANCIO IN RING 0 (KERNEL MODE / VECCHIE APP PIATTE) ===
        // Per restare in Ring 0, IRET pretende solo 3 valori
        *(--st) = 0x0202;               // EFLAGS
        uint16_t cs; asm volatile("mov %%cs, %0" : "=r"(cs));
        *(--st) = (uint32_t)cs;         // CS
        *(--st) = (uint32_t)entry_point;// EIP
    }

    
    // 3. I Registri Generali: i valori esatti che il comando 'popal' si aspetta!
    // Vanno inseriti in ordine opposto (EAX in cima, EDI in fondo)
    *(--st) = 0; // EAX
    *(--st) = 0; // ECX
    *(--st) = 0; // EDX
    *(--st) = 0; // EBX
    *(--st) = 0; // ESP (ignorato dal popal, ma lo spazio serve)
    *(--st) = 0; // EBP
    *(--st) = 0; // ESI
    *(--st) = 0; // EDI

    // 4. NUOVO: I Registri di Segmento (Vitali per non far crashare il Ring 3)
    uint32_t data_seg = is_virtual ? 0x23 : 0x10; // 0x23 per Utenti, 0x10 per Kernel
    *(--st) = data_seg; // DS
    *(--st) = data_seg; // ES
    *(--st) = data_seg; // FS
    *(--st) = data_seg; // GS

    // Salviamo la posizione in cui siamo arrivati
    process_table[pid].esp = (uint32_t)st;
    process_table[pid].active = 1;
    
    process_table[pid].waiting_for_input = 0;
    return pid;
}


// ==========================================
// GDT (GLOBAL DESCRIPTOR TABLE) E RING 3
// ==========================================

struct gdt_entry {
    uint16_t limit_low; uint16_t base_low; uint8_t base_middle;
    uint8_t access; uint8_t granularity; uint8_t base_high;
} __attribute__((packed));

struct gdt_ptr {
    uint16_t limit; uint32_t base;
} __attribute__((packed));

struct gdt_entry gdt[6]; // ORA SONO 6 CARTE D'IDENTITà
struct gdt_ptr gdtp;

void gdt_set_gate(int num, uint32_t base, uint32_t limit, uint8_t access, uint8_t gran) {
    gdt[num].base_low = (base & 0xFFFF); gdt[num].base_middle = (base >> 16) & 0xFF;
    gdt[num].base_high = (base >> 24) & 0xFF; gdt[num].limit_low = (limit & 0xFFFF);
    gdt[num].granularity = (limit >> 16) & 0x0F; gdt[num].granularity |= gran & 0xF0;
    gdt[num].access = access;
}

// STRUTTURA DEL TSS (Il ponte di emergenza per il Ring 0)
struct tss_entry_struct {
    uint32_t prev_tss;
    uint32_t esp0;     // LO STACK DI EMERGENZA (Kernel Stack)
    uint32_t ss0;      // Il segmento dati del Kernel
    uint32_t esp1, ss1, esp2, ss2, cr3, eip, eflags, eax, ecx, edx, ebx, esp, ebp, esi, edi, es, cs, ss, ds, fs, gs, ldt, trap, iomap_base;
} __attribute__((packed));

struct tss_entry_struct tss_entry;

void write_tss(int32_t num, uint16_t ss0, uint32_t esp0) {
    uint32_t base = (uint32_t) &tss_entry;
    uint32_t limit = sizeof(tss_entry);
    gdt_set_gate(num, base, limit, 0xE9, 0x00);
    for(size_t i=0; i<sizeof(tss_entry); i++) ((uint8_t*)&tss_entry)[i] = 0;
    tss_entry.ss0  = ss0;
    tss_entry.esp0 = esp0;
    tss_entry.iomap_base = sizeof(tss_entry);
}

void init_gdt() {
    gdtp.limit = (sizeof(struct gdt_entry) * 6) - 1; 
    gdtp.base  = (uint32_t)&gdt;

    gdt_set_gate(0, 0, 0, 0, 0); // Nullo
    gdt_set_gate(1, 0, 0xFFFFFFFF, 0x9A, 0xCF); // Codice Kernel (Ring 0)
    gdt_set_gate(2, 0, 0xFFFFFFFF, 0x92, 0xCF); // Dati Kernel (Ring 0)
    gdt_set_gate(3, 0, 0xFFFFFFFF, 0xFA, 0xCF); // Codice App (Ring 3)
    gdt_set_gate(4, 0, 0xFFFFFFFF, 0xF2, 0xCF); // Dati App (Ring 3)
    write_tss(5, 0x10, 0x0); // TSS (Entry 5)

    asm volatile(
        "lgdt %0 \n"
        "mov $0x10, %%ax \n"
        "mov %%ax, %%ds \n"
        "mov %%ax, %%es \n"
        "mov %%ax, %%fs \n"
        "mov %%ax, %%gs \n"
        "mov %%ax, %%ss \n"
        "ljmp $0x08, $1f \n"
        "1: \n"
        "mov $0x2B, %%ax \n" // 0x28 (Entry 5) + 3 (RPL) = 0x2B
        "ltr %%ax \n"        // Installiamo il TSS nella CPU
        : : "m"(gdtp)
    );
}



// ==========================================
// TERMINAZIONE FORZATA DELLE APP (Crash)
// ==========================================
void terminate_faulting_app(const char* reason) {
    print_term(reason); 

    // Come ogni altra chiusura: anche le finestre dell'app se ne vanno,
    // invece di restare sullo schermo senza piu' un processo dietro
    close_app(current_pid);
    app_wait_for_end();
}



// ==========================================
// SCHERMATA BLU DELLA MORTE (KERNEL PANIC)
// ==========================================
void kernel_panic(const char* err_msg) {
    // 1. CHIUDIAMO LE PORTE: Niente più timer, niente più mouse. Il tempo si ferma.
    asm volatile("cli");

    // 2. Dipingiamo brutalmente tutta la RAM video di Blu Scuro
    for (int i = 0; i < fb_width * fb_height; i++) {
        backbuffer[i] = 0xFF0000AA; 
    }

    // 3. Disegniamo il testo di errore bypassando il Window Manager
    draw_string("==================================================", 50, 50, 0xFFFFFFFF, 2);
    draw_string("                   KERNEL PANIC                   ", 50, 80, 0xFFFFFFFF, 2);
    draw_string("==================================================", 50, 110, 0xFFFFFFFF, 2);
    
    draw_string("Un errore fatale ha bloccato il sistema.", 50, 160, 0xFFFFFFFF, 1);
    draw_string("Motivo dell'arresto:", 50, 190, 0xFFFFFFFF, 1);
    
    draw_string(err_msg, 50, 220, 0xFFFFFF00, 2); // Il messaggio di errore in Giallo
    
    draw_string("Sistema arrestato per prevenire corruzione dei dati.", 50, 280, 0xFFFFFFFF, 1);
    draw_string("Riavviare fisicamente la macchina.", 50, 300, 0xFFFFFFFF, 1);

    swap_buffers();

    // 4. LA MORTE DELLA CPU: Un ciclo infinito in cui la CPU si mette a dormire (Halt)
    while(1) {
        asm volatile("hlt");
    }
}

// --- FUNZIONI C INTELLIGENTI CHE LEGGONO IL RING ---
// Il parametro 'cs' ci viene passato direttamente dall'Assembly

void panic_generic(uint32_t cs) { 
    if ((cs & 3) == 3) terminate_faulting_app("ERRORE APP: Eccezione Hardware. Terminata.");
    else kernel_panic("Eccezione Hardware Non Gestita nel Kernel"); 
}

void panic_divzero(uint32_t cs) { 
    // L'operatore bitwise '& 3' isola gli ultimi 2 bit del registro CS
    if ((cs & 3) == 3) terminate_faulting_app("ERRORE APP: Divisione per Zero! Processo terminato.");
    else kernel_panic("Interrupt 0x00: Divisione per Zero nel Kernel!"); 
}

void panic_gpf(uint32_t cs) { 
    if ((cs & 3) == 3) terminate_faulting_app("ERRORE APP: Violazione di Memoria (GPF)! Terminata.");
    else kernel_panic("Interrupt 0x0D: General Protection Fault nel Kernel!"); 
}

// ==========================================
// GESTORE DEL PAGE FAULT (Interrupt 14)
// ==========================================
void panic_pagefault(uint32_t cs) { 
    uint32_t cr2_val;
    asm volatile("mov %%cr2, %0" : "=r" (cr2_val));
    
    // IL CUORE DEL DEMAND PAGING
    if (cr2_val >= 0x40000000 && cr2_val < 0x40400000) {
        
        // Chiediamo alla CPU quale "Mappa" è correntemente in uso
        // Smascheriamo gli ultimi bit per avere l'indirizzo fisico puro
        uint32_t active_pt_addr = page_directory[256] & 0xFFFFF000;
        
        // Se c'è una mappa montata, cuciamo la RAM su quella
        if (active_pt_addr != 0) {
            uint32_t physical_frame = pfa_alloc_frame();
            if (physical_frame == 0) {
                terminate_faulting_app("ERRORE OOM: Memoria RAM Fisica esaurita!");
                return;
            }

            uint8_t* frame_ptr = (uint8_t*)physical_frame;
            for(int i = 0; i < 4096; i++) {
                frame_ptr[i] = 0;
            }
            
            uint32_t pt_index = (cr2_val - 0x40000000) / 4096;
            
            // Usiamo direttamente il puntatore hardware
            uint32_t* active_pt = (uint32_t*)active_pt_addr;
            active_pt[pt_index] = physical_frame | 0x07;
            
            asm volatile("invlpg (%0)" :: "r" (cr2_val) : "memory");
            return; 
        }
    }

    // Se arriviamo qui, il Page Fault non era previsto...
    if ((cs & 3) == 3) {
        char msg[100] = "ERRORE APP: Violazione di Memoria a: ";
        char num[32]; itoa(cr2_val, num);
        int i = 0; while(msg[i]) i++;
        int j = 0; while(num[j]) msg[i++] = num[j++]; msg[i] = '\0';
        terminate_faulting_app(msg);
    } else {
        char msg[100] = "KERNEL PANIC: Page Fault all'indirizzo: ";
        char num[32]; itoa(cr2_val, num);
        int i = 0; while(msg[i]) i++;
        int j = 0; while(num[j]) msg[i++] = num[j++]; msg[i] = '\0';
        kernel_panic(msg); 
    }
}

// --- Leggono il CS dallo Stack della CPU ---
// In x86, se non c'è l'Error Code, il CS è distante 4 byte dall'ESP.
// Se c'è l'Error Code (come nel Page Fault o GPF), il CS è distante 8 byte.

__attribute__((naked)) void isr_generic() { 
    asm volatile("mov 4(%esp), %eax \n push %eax \n call panic_generic"); 
}
__attribute__((naked)) void isr0_divzero() { 
    asm volatile("mov 4(%esp), %eax \n push %eax \n call panic_divzero"); 
}
__attribute__((naked)) void isr13_gpf() { 
    asm volatile("mov 8(%esp), %eax \n push %eax \n call panic_gpf"); 
}
__attribute__((naked)) void isr14_pfault() { 
    asm volatile(
        "pushal \n"               // 1. Salva tutti i registri per non corrompere l'App
        
        // 2. Troviamo il CS: 32 byte (pushal) + 4 byte (Error Code) + 4 byte (EIP) = Offset 40
        "mov 40(%esp), %eax \n"   
        "push %eax \n"            // 3. Passa il CS come argomento alla funzione C
        
        "call panic_pagefault \n" // 4. Esegui la magia del Demand Paging
        
        "add $4, %esp \n"         // 5. Rimuovi l'argomento (CS) dallo Stack
        "popal \n"                // 6. Ripristina intatti i registri dell'App
        
        "add $4, %esp \n"         // 7. Rimuovi l'Error Code spinto dalla CPU
        "iret \n"                 // 8. Ritorno trionfale all'istruzione crashata!
    ); 
}

// La singola "voce" nella rubrica telefonica della CPU
struct idt_entry {
    uint16_t base_low;   // Parte bassa dell'indirizzo della nostra funzione C
    uint16_t sel;        // Il segmento di codice (Code Segment)
    uint8_t  zero;       // Sempre zero
    uint8_t  flags;      // Permessi (Chi può suonare questo campanello?)
    uint16_t base_high;  // Parte alta dell'indirizzo
} __attribute__((packed));

// Il puntatore speciale che daremo in pasto alla CPU
struct idt_ptr {
    uint16_t limit;
    uint32_t base;
} __attribute__((packed));

struct idt_entry idt[256];
struct idt_ptr idtp;

// Struttura fittizia richiesta da GCC per le interruzioni
struct interrupt_frame; 




// --- IL BATTITO CARDIACO E LO SCHEDULER (Interrupt 32) ---
volatile uint32_t current_esp;

volatile char last_key_pressed = 0;

// Il Direttore d'Orchestra: sceglie chi deve suonare ora
void schedule() {
    system_ticks++;
    
    // --- MOTORE DEL LAMPEGGIO CURSORE ---
    blink_counter++;
    if (blink_counter >= 100) blink_counter = 0; // Si resetta ogni secondo

    // Motore di animazione legato all'orologio hardware (100 FPS)
    if (anim_active) {
        // Calcoliamo quanti fotogrammi sono passati dall'inizio
        int elapsed = anim_frames_total - anim_frames_left + 1;
        
        // INTERPOLAZIONE LINEARE: Posizione = Partenza + ((Distanza * Tempo_Trascorso) / Tempo_Totale)
        // Moltiplicando prima di dividere, non perdiamo MAI un singolo pixel di precisione
        anim_x = anim_start_x + ((anim_target_x - anim_start_x) * elapsed) / anim_frames_total;
        anim_y = anim_start_y + ((anim_target_y - anim_start_y) * elapsed) / anim_frames_total;
        anim_w = anim_start_w + ((anim_target_w - anim_start_w) * elapsed) / anim_frames_total;
        anim_h = anim_start_h + ((anim_target_h - anim_start_h) * elapsed) / anim_frames_total;
        
        anim_frames_left--;
        
        if (anim_frames_left <= 0 || anim_w <= 0 || anim_h <= 0) anim_active = 0;
        
        refresh_requested = 1; // Forza il ridisegno del desktop a 100 Frame al Secondo
    } 
    // Il ritmo veloce serve solo alle particelle dei vassoi, che esistono
    // unicamente sulle finestre a dimensione normale. Se non ce n'e' nessuna
    // visibile si torna al ritmo di riposo, che basta al cursore di testo.
    else {
        int want_fast = 1;   // il vassoio della taskbar e' sempre animato
        for (int i = 0; i < MAX_WINDOWS; i++) {
            if (!windows[i].active || windows[i].is_minimized) continue;
            if (windows[i].is_maximized) continue;
            if (windows[i].type == WIN_TYPE_TERMINAL) continue;
            if (windows[i].type == WIN_TYPE_START_MENU) continue;
            if (windows[i].type == WIN_TYPE_APP_MENU) continue;
            want_fast = 1;
            break;
        }
        if (system_ticks % (want_fast ? 6 : 50) == 0) refresh_requested = 1;
    }

        // --- ANIMAZIONE DELLA TASKBAR FLUTTUANTE, LARGHEZZA DINAMICA E MENU ---
    int anim_tb_changed = 0;
    
    // A. Animazione Verticale
    if (taskbar_y_offset != taskbar_target_offset) {
        if (taskbar_y_offset < taskbar_target_offset) taskbar_y_offset++;
        else taskbar_y_offset--;
        anim_tb_changed = 1;
    }

    // B. Animazione Orizzontale (Espansione/Compressione fluida e rapida)
    if (taskbar_w != taskbar_target_w) {
        if (taskbar_w < taskbar_target_w) {
            taskbar_w += 16; // Passo di espansione
            if (taskbar_w > taskbar_target_w) taskbar_w = taskbar_target_w;
        } else {
            taskbar_w -= 16; // Passo di compressione
            if (taskbar_w < taskbar_target_w) taskbar_w = taskbar_target_w;
        }
        anim_tb_changed = 1;
    }

    if (anim_tb_changed) {
        refresh_requested = 1; // Ridisegna in modo fluido
        
        // Sincronizzazione Magnetica: Spostiamo i Menu aperti insieme alla barra (sia Y che X dinamica!)
        int start_menu_y = -1;
        int start_menu_x = -1;
        for (int m = 0; m < MAX_WINDOWS; m++) {
            if (windows[m].active && windows[m].type == WIN_TYPE_START_MENU) {
                windows[m].y = taskbar_panel_top() - windows[m].h;
                // Il Menu resta incollato al pulsante Start che si sposta con la barra centrata
                windows[m].x = ((fb_width - taskbar_w) / 2) + 7; 
                start_menu_y = windows[m].y;
                start_menu_x = windows[m].x;
            }
        }
        if (start_menu_y != -1) {
            for (int m = 0; m < MAX_WINDOWS; m++) {
                if (windows[m].active && windows[m].type == WIN_TYPE_APP_MENU) {
                    windows[m].y = start_menu_y; 
                    windows[m].x = start_menu_x + 160; // Il sottomenu segue a destra del menu Start
                }
            }
        }
    }


    // Salviamo l'ESP nel processo che stiamo mettendo in pausa
    process_table[current_pid].esp = current_esp;

    // Cerchiamo il prossimo processo (Round Robin)
    do {
        current_pid++;
        if (current_pid >= MAX_PROCESSES) current_pid = 0;
        
    
    
    // Saltiamo non solo i processi inattivi, ma ANCHE quelli che stanno dormendo in attesa di input/eventi
    } while (process_table[current_pid].active == 0 || process_table[current_pid].waiting_for_input > 0);

    // Carichiamo l'ESP del nuovo processo
    current_esp = process_table[current_pid].esp;

    // TSS: Diciamo alla CPU dove si trova lo Stack di emergenza di QUESTO specifico processo
    tss_entry.esp0 = process_table[current_pid].stack_base + 4096;

    // RIMAPPATURA DELLA MEMORIA VIRTUALE
    if (current_pid > 0) {
        // Colleghiamo l'indirizzo Virtuale alla Tabella PRIVATA dell'App
        page_directory[256] = ((uint32_t)process_table[current_pid].page_table) | 0x07; 
    } else {
        // Se è il Kernel a girare, stacchiamo la memoria delle App
        page_directory[256] = 0; 
    }
    // Flush TLB (Diciamo al processore di aggiornare la mappa della memoria)
    asm volatile("mov %%cr3, %%eax; mov %%eax, %%cr3" ::: "eax");
}

// L'Interrupt Handler Naked: Scambia fisicamente TUTTI i registri
__attribute__((naked)) void isr32_timer() {
    asm volatile(
        "pushal \n"                // Salva i registri generali
        "push %ds \n"              // Salva i segmenti dati
        "push %es \n"
        "push %fs \n"
        "push %gs \n"
        "mov %esp, current_esp \n" // Salva lo stato attuale
        "call schedule \n"         // Cambia il processo
        "mov current_esp, %esp \n" // Carica il nuovo stato
        "mov $0x20, %al \n"
        "out %al, $0x20 \n"        // Conferma al PIC
        "pop %gs \n"               // Ripristina i segmenti!
        "pop %fs \n"
        "pop %es \n"
        "pop %ds \n"
        "popal \n"                 // Ripristina i registri generali
        "iret \n"                  
    );
}


// ============================================================================
// IL ROBOTTINO COME PERMESSO DI ACCESSO ALL'INTELLIGENZA ARTIFICIALE
// Un'applicazione puo' interrogare l'IA solo se l'utente le ha portato il
// robottino sul vassoio della sua barra del titolo: il permesso e' un gesto
// fisico, non una casella da spuntare. Il controllo sta QUI e non nell'app,
// altrimenti un programma scritto male, o in malafede, potrebbe saltarlo.
// Il robottino e' uno solo, quindi c'e' una sola richiesta per volta.
//
// La richiesta e' una POST /api/chat a Ollama, con "stream": true. La risposta
// arriva come una sequenza di righe JSON, ciascuna con un pezzetto di testo:
// li estraiamo e li scriviamo nell'area dell'app man mano che arrivano, cosi'
// il testo compare parola per parola invece che tutto insieme alla fine.
// Niente di tutto questo blocca il sistema: si avanza un po' per fotogramma.
// ============================================================================
#define ROBOT_ASSENTE 0    // il robottino non e' su questa finestra
#define ROBOT_PRONTO  1    // presente e libero
#define ROBOT_PENSA   2    // richiesta in corso
#define ROBOT_ERRORE  3

#define ROBOT_MODEL  "qwen3.5:2b"    // il modello installato in Ollama
#define ROBOT_PORT   11434

#define ROBOT_PROMPT_MAX 1024
#define ROBOT_REQ_MAX    3072
#define ROBOT_CONN_TO    500     // 5 s per aprire la connessione
#define ROBOT_IDLE_TO    18000   // 3 minuti senza un byte: il modello ci mette
                                 // molto solo la prima volta, quando lo carica

// Stati della connessione TCP, ripetuti identici a come sono definiti piu'
// avanti: il C lo consente, e ci permette di tenere il robottino qui sopra.
#define TCP_CHIUSA   0
#define TCP_ATTESA   1
#define TCP_APERTA   2
#define TCP_FINITA   3
#define TCP_ERRORE   4

// Funzioni della pila di rete, definite piu' avanti nel file
int  tcp_connect(const uint8_t* ip, uint16_t port);
int  tcp_send(const char* data, int len);
int  tcp_state(void);
int  tcp_rx_size(void);
const char* tcp_rx_data(void);
void tcp_rx_consume(int n);
void net_poll(void);
static const uint8_t net_gw_ip[4];

static int robot_state = ROBOT_PRONTO;
static int robot_win   = -1;   // finestra che ha la richiesta in corso
static int robot_area  = 1;    // area di testo in cui scrivere la risposta
static int robot_phase = 0;    // 1 = connessione, 2 = risposta in arrivo
static int robot_parsed = 0;   // byte del flusso gia' esaminati
static int robot_first_line = 1;
static char robot_prompt[ROBOT_PROMPT_MAX + 1];
static char robot_req[ROBOT_REQ_MAX];
static int  robot_req_len = 0;
static uint32_t robot_t0 = 0;      // inizio della connessione
static uint32_t robot_last = 0;    // ultimo byte ricevuto

// Scrive in coda a un'area di testo di una finestra, come fa la syscall 22
static void ta_append_win(int win_id, int idx, const char* s) {
    if (win_id < 0 || win_id >= MAX_WINDOWS || idx < 0 || idx >= TA_MAX) return;
    textarea_t* ta = &windows[win_id].ta[idx];
    if (!ta->buf) ta_alloc(ta);
    if (!ta->buf) return;
    ta_append_text(ta, s);                // UTF-8 (anche da Ollama) -> Latin-1
    ta->buf[ta->len] = '\0';
    ta->cursor = ta->len;
    ta->scroll = 1 << 20;      // il disegno la riporta all'ultima pagina
    ta->version++;
    refresh_requested = 1;
}

// Il permesso: il robottino deve abitare proprio su questa finestra
static int robot_permesso(int win_id) {
    return (win_id >= 0 && win_id < MAX_WINDOWS && windows[win_id].active &&
            mascot_host == win_id);
}

// Sveglia l'applicazione che aspetta: serve quando la richiesta finisce,
// perche' la scritta "Sta pensando..." la disegna lei e va aggiornata.
static void robot_wake_app(void) {
    if (robot_win < 0 || robot_win >= MAX_WINDOWS) return;
    int pid = windows[robot_win].owner_pid;
    if (pid > 0 && pid < MAX_PROCESSES && process_table[pid].active &&
        process_table[pid].waiting_for_input == 2)
        process_table[pid].waiting_for_input = 0;
}

static void robot_reset(void) {
    robot_wake_app();
    robot_state = ROBOT_PRONTO;
    robot_win = -1;
    robot_phase = 0;
    robot_parsed = 0;
    robot_first_line = 1;
}

// Compone la richiesta HTTP con dentro il JSON. Il testo dell'utente va
// protetto: virgolette e barre rovesciate hanno un significato nel JSON, e
// lasciarle passare tali e quali romperebbe il messaggio.
static void robot_build_request(const char* prompt) {
    static char body[ROBOT_REQ_MAX];   // fuori dallo stack del kernel, che e' di 4 KB
    int b = 0;

    const char* p1 = "{\"model\":\"" ROBOT_MODEL "\",\"stream\":true,"
                     "\"messages\":[{\"role\":\"user\",\"content\":\"";
    const char* p2 = "\"}]}";
    for (int i = 0; p1[i]; i++) body[b++] = p1[i];

    for (int i = 0; prompt[i] && b < ROBOT_REQ_MAX - 16; i++) {
        unsigned char c = (unsigned char)prompt[i];
        if      (c == '"')  { body[b++] = '\\'; body[b++] = '"';  }
        else if (c == '\\') { body[b++] = '\\'; body[b++] = '\\'; }
        else if (c == '\n') { body[b++] = '\\'; body[b++] = 'n';  }
        else if (c == '\t') { body[b++] = '\\'; body[b++] = 't';  }
        else if (c >= 32 && c < 127) body[b++] = (char)c;
        else if (c >= 0xA0) {              // Latin-1 -> UTF-8, come vuole il JSON
            body[b++] = (char)(0xC0 | (c >> 6));
            body[b++] = (char)(0x80 | (c & 0x3F));
        }
        else body[b++] = ' ';              // caratteri di controllo: uno spazio
    }
    for (int i = 0; p2[i]; i++) body[b++] = p2[i];
    body[b] = '\0';

    // Intestazione, con la lunghezza del corpo contata davvero
    char num[12];
    int nl = 0, v = b;
    do { num[nl++] = (char)('0' + v % 10); v /= 10; } while (v);

    const char* h1 = "POST /api/chat HTTP/1.1\r\nHost: 10.0.2.2:11434\r\n"
                     "Content-Type: application/json\r\nContent-Length: ";
    const char* h2 = "\r\nConnection: close\r\n\r\n";

    int r = 0;
    for (int i = 0; h1[i]; i++) robot_req[r++] = h1[i];
    while (nl) robot_req[r++] = num[--nl];
    for (int i = 0; h2[i]; i++) robot_req[r++] = h2[i];
    for (int i = 0; i < b; i++) robot_req[r++] = body[i];
    robot_req_len = r;
}

// Estrae il testo da una riga JSON di Ollama e lo scrive nell'area.
// Cerchiamo direttamente il campo "content": le righe delle intestazioni HTTP
// e quelle con la lunghezza dei blocchi non lo contengono, quindi passano oltre
// da sole senza bisogno di interpretare tutto il protocollo.
static int robot_line(const char* s, int len) {
    static const char key[] = "\"content\":\"";
    int done = 0;

    for (int i = 0; i + 11 <= len; i++) {
        int m = 1;
        for (int k = 0; k < 11; k++) if (s[i + k] != key[k]) { m = 0; break; }
        if (!m) continue;

        int j = i + 11;
        char out[256];
        int o = 0;
        while (j < len && s[j] != '"') {
            char c = s[j];
            if (c == '\\' && j + 1 < len) {
                j++;
                char e = s[j];
                if      (e == 'n') c = '\n';
                else if (e == 't') c = ' ';
                else if (e == 'r') { j++; continue; }
                else if (e == 'u' && j + 4 < len) {
                    // \uXXXX: lo si riscrive in UTF-8, e ta_append_win lo
                    // convertira' in Latin-1 insieme a tutto il resto
                    uint32_t cp = 0;
                    for (int h = 1; h <= 4; h++) {
                        char x = s[j + h];
                        cp = cp * 16 + (uint32_t)((x >= '0' && x <= '9') ? x - '0' : (x | 32) - 'a' + 10);
                    }
                    j += 5;
                    if (cp >= 0xD800 && cp <= 0xDFFF) continue;     // meta' di un'emoji
                    if (cp < 0x80) {
                        if (o < 250) out[o++] = (char)cp;
                    } else if (cp < 0x800) {
                        if (o < 249) { out[o++] = (char)(0xC0 | (cp >> 6));
                                       out[o++] = (char)(0x80 | (cp & 0x3F)); }
                    } else if (o < 248) {
                        out[o++] = (char)(0xE0 | (cp >> 12));
                        out[o++] = (char)(0x80 | ((cp >> 6) & 0x3F));
                        out[o++] = (char)(0x80 | (cp & 0x3F));
                    }
                    continue;
                }
                else c = e;                    // virgolette, barre, eccetera
            }
            if (o < 250) out[o++] = c;
            j++;
        }
        out[o] = '\0';
        if (o > 0) ta_append_win(robot_win, robot_area, out);
        i = j;
    }

    // "done":true chiude la risposta
    for (int i = 0; i + 11 <= len; i++) {
        static const char dk[] = "\"done\":true";
        int m = 1;
        for (int k = 0; k < 11; k++) if (s[i + k] != dk[k]) { m = 0; break; }
        if (m) { done = 1; break; }
    }
    return done;
}

// Esamina i byte arrivati, una riga completa per volta: una riga a meta'
// resta in attesa del resto.
static int robot_consume(void) {
    int n = tcp_rx_size();
    const char* d = tcp_rx_data();
    int done = 0;

    while (robot_parsed < n) {
        int e = robot_parsed;
        while (e < n && d[e] != '\n') e++;
        if (e >= n) break;                       // riga incompleta

        int len = e - robot_parsed;
        if (len > 0 && d[robot_parsed + len - 1] == '\r') len--;

        if (robot_first_line) {
            robot_first_line = 0;
            // La prima riga e' l'esito HTTP: se non e' 200, lo mostriamo
            int ok = 0;
            for (int i = 0; i + 3 <= len; i++)
                if (d[robot_parsed + i] == '2' && d[robot_parsed + i + 1] == '0' &&
                    d[robot_parsed + i + 2] == '0') { ok = 1; break; }
            if (!ok) {
                ta_append_win(robot_win, robot_area, "[Ollama ha risposto: ");
                char tmp[128];
                int t = 0;
                for (int i = 0; i < len && t < 120; i++) tmp[t++] = d[robot_parsed + i];
                tmp[t] = '\0';
                ta_append_win(robot_win, robot_area, tmp);
                ta_append_win(robot_win, robot_area, "]");
                done = 1;
            }
        } else if (len > 0) {
            if (robot_line(&d[robot_parsed], len)) done = 1;
        }

        robot_parsed = e + 1;
        if (done) break;
    }

    // Quello che abbiamo letto si butta: cosi' il buffer non si riempie mai
    if (robot_parsed > 0) {
        tcp_rx_consume(robot_parsed);
        robot_parsed = 0;
    }
    if (n > 0) robot_last = system_ticks;
    return done;
}

// Fa avanzare la richiesta. Viene richiamata quando l'app chiede lo stato,
// cioe' a ogni suo fotogramma: nessuna attesa bloccante.
static void robot_tick(void) {
    if (robot_state != ROBOT_PENSA) return;

    // Il permesso decade se il robottino lascia la finestra a meta' strada
    if (!robot_permesso(robot_win)) {
        ta_append_win(robot_win, robot_area, "\n[Il robottino se n'e' andato: richiesta interrotta]");
        robot_reset();
        return;
    }

    net_poll();      // fa girare la pila di rete

    if (robot_phase == 1) {
        int st = tcp_state();
        if (st == TCP_APERTA) {
            tcp_send(robot_req, robot_req_len);
            robot_phase = 2;
            robot_last = system_ticks;
        } else if (st == TCP_ERRORE || system_ticks - robot_t0 > ROBOT_CONN_TO) {
            ta_append_win(robot_win, robot_area,
                          "[Nessuna risposta da Ollama: e' in esecuzione sul Mac?]");
            robot_reset();
        }
        return;
    }

    if (robot_phase == 2) {
        int done = robot_consume();
        int st = tcp_state();

        if (done || st == TCP_FINITA) {
            robot_reset();
        } else if (st == TCP_ERRORE) {
            ta_append_win(robot_win, robot_area, "\n[Connessione interrotta]");
            robot_reset();
        } else if (system_ticks - robot_last > ROBOT_IDLE_TO) {
            ta_append_win(robot_win, robot_area, "\n[Tempo scaduto]");
            robot_reset();
        }
    }
}

// --- IL VERO GESTORE DELLE SYSCALL ---
// Adesso riceve i parametri direttamente da Assembly
void syscall_handler(uint32_t sys_id, uint32_t arg1, uint32_t arg2, uint32_t arg3) {

    if (sys_id == 1) {
        print_term((const char*)arg1);
        // INVECE DI FORZARE IL DISEGNO, ALZIAMO SOLO LA BANDIERINA
        // Sarà il ciclo main() a disegnare lo schermo quando ha tempo, fluidificando tutto
        refresh_requested = 1; 
    }
    else if (sys_id == 2) {
        for(int i=0; i<TERM_MAX_ROWS; i++) term_history[i][0] = '\0'; 
        term_scroll = 0;
        render_desktop(mouse_x, mouse_y, is_clicking);
    }
    else if (sys_id == 3) {
        // Diciamo al Kernel di cosa abbiamo bisogno
        process_table[current_pid].input_buffer = (char*)arg1;
        process_table[current_pid].input_max = arg2;
        process_table[current_pid].input_idx = 0;
        process_table[current_pid].waiting_for_input = 1;
        
        print_term("> "); 
        
        // Diciamo alla CPU di ricominciare ad ascoltare il Timer e la tastiera
        asm volatile("sti");
        
        // L'app si addormenta. Ora il Timer PUÒ interromperla per far scorrere il Multitasking
        while(process_table[current_pid].waiting_for_input == 1) {
            asm volatile("pause"); 
        }
    }
    else if (sys_id == 4) {
        process_table[current_pid].is_critical_io = 1; // ALZIAMO LO SCUDO
        // CODA DI ATTESA: Se il disco è bloccato (es. copia in corso), riaccendi
        // gli interrupt, aspetta un millisecondo e ritenta (Spinlock con Yield)
        while (vfs_lock == 1) {
            asm volatile("sti \n pause \n cli"); 
        }
        uint32_t file_size = arg3;
        vfs_lock = 1; // Chiudiamo a nostra volta il lucchetto

        // Passiamo il file_size alla funzione del file system
        if (ext2_write_file((const char*)arg1, (const char*)arg2, file_size)) {
            print_term("API: File salvato su disco!");
        } else {
            print_term("API: ERRORE! Impossibile salvare il file.");
        }
        vfs_lock = 0; // Apriamo il lucchetto
        process_table[current_pid].is_critical_io = 0; // ABBASSIAMO LO SCUDO
        render_desktop(mouse_x, mouse_y, is_clicking);
    }

    // ========================================================
    // SYSCALL 5: LETTURA EVENTI MOUSE (Multi-Window Aware)
    // ========================================================
    else if (sys_id == 5) {
        mouse_state_t* app_mouse = (mouse_state_t*)arg1;

        if (current_pid > 0) {
            int top_win = window_order[0];
            
            // Controlliamo se la finestra in cima APPARTIENE a questa App
            if (windows[top_win].active && windows[top_win].owner_pid == current_pid && windows[top_win].type == WIN_TYPE_APP_GUI) {
                
                // === FIX: IL RITORNO DELLO ZERO MAGICO ===
                // Se la finestra colpita è la madre, la mascheriamo come '0' per l'SDK in Ring 3!
                if (top_win == process_table[current_pid].linked_window) {
                    app_mouse->win_id = 0;
                } else {
                    app_mouse->win_id = top_win; // Per i pop-up usiamo il vero ID
                }
                
                app_mouse->clicked = is_clicking;
                
                app_mouse->x = mouse_x - (windows[top_win].x + 4);
                app_mouse->y = mouse_y - (windows[top_win].y + 30);
                
                // Clic fuori dal canvas (es. barra del titolo)? Ignoriamo per l'App
                if (app_mouse->x < 0 || app_mouse->y < 0 || 
                    app_mouse->x > (windows[top_win].w - 8) || app_mouse->y > (windows[top_win].h - 34)) {
                    app_mouse->clicked = 0; 
                }
            } else {
                app_mouse->clicked = 0;
                app_mouse->win_id = -1; // Nessuna nostra finestra a fuoco
            }
        }
    }
    // ========================================================
    // SYSCALL 6: YIELD (Attesa Eventi GUI)
    // ========================================================
    else if (sys_id == 6) {
        process_table[current_pid].waiting_for_input = 2; // 2 = Sospeso fino a evento Mouse!
        asm volatile("sti");
        while(process_table[current_pid].waiting_for_input == 2) {
            asm volatile("pause"); 
        }
    }

    // ========================================================
    // SYSCALL 7: DISEGNO INTERFACCIA (Retained Mode)
    // ========================================================
    else if (sys_id == 7) {
        gui_element_t* app_gui = (gui_element_t*)arg1;
        int target_win_id = (int)arg2; // ORA LE APP POSSONO SCEGLIERE LA TELA!
        
        if (current_pid > 0) {
            int win_id = target_win_id;
            // Se l'app usa le vecchie funzioni (arg2 == 0 o -1), usiamo la finestra madre
            if (win_id <= 0) win_id = process_table[current_pid].linked_window;
            
            // VERIFICA SICUREZZA: L'app possiede davvero questa finestra?
            if (win_id >= 0 && win_id < MAX_WINDOWS && windows[win_id].owner_pid == current_pid) {
                if (app_gui->type == 0) {
                    windows[win_id].ui_count = 0; // Se Type 0, svuotiamo la tela
                    
                    // L'app ha iniziato a disegnare: rendiamo visibile la finestra!
                    windows[win_id].is_initializing = 0; 

                    // Reset del pavimento, da cui ripartira' la crescita sui contenuti.
                    // Se lo sviluppatore ha scelto un minimo vale quello; altrimenti la
                    // misura d'apertura, come e' sempre stato.
                    if (windows[win_id].dev_min_w > 0) {
                        windows[win_id].min_w = windows[win_id].dev_min_w;
                        windows[win_id].min_h = windows[win_id].dev_min_h;
                    } else {
                        windows[win_id].min_w = windows[win_id].base_w;
                        windows[win_id].min_h = windows[win_id].base_h;
                    }
                }  
                else if (windows[win_id].ui_count < 32) {
                    
                    // --- DECODIFICA DEI FLAG ---
                    int incoming_type = app_gui->type;
                    int base_type = incoming_type & 0xFF;  // Mantiene solo il tipo base (0-5)
                    int ignore_w = incoming_type & 0x0100; // Flag: Evita espansione X
                    int ignore_h = incoming_type & 0x0200; // Flag: Evita espansione Y

                    gui_element_t* dest = &windows[win_id].ui_elements[windows[win_id].ui_count];
                    
                    // FIX GRASSETTO: Preserviamo il bit 0x0400 (Bold) insieme al tipo base
                    dest->type = base_type | (incoming_type & 0x0400); 
                    
                    dest->x = app_gui->x; dest->y = app_gui->y;
                    dest->w = app_gui->w; dest->h = app_gui->h;
                    dest->color1 = app_gui->color1; 
                    dest->color2 = app_gui->color2; 
                    
                    if (base_type == 5) {
                        dest->color1 = app_gui->color1;
                    }

                    // Il testo dell'app e' UTF-8: dentro il sistema diventa Latin-1
                    utf8_to_latin1(dest->text, 256, app_gui->text, 256);
                    windows[win_id].ui_count++;



                    // Area di testo: il suo buffer vive nel kernel e si crea al
                    // primo fotogramma in cui l'app la dichiara.
                    if (base_type == 6) {
                        int ti = dest->color2 & 0xFF;
                        if (ti < TA_MAX && !windows[win_id].ta[ti].buf)
                            ta_alloc(&windows[win_id].ta[ti]);
                    }
                    // ===================================================
                    // IL CUORE DEL CONTENT-AWARE WINDOW SIZING
                    // ===================================================
                    int el_w = dest->w;
                    int el_h = dest->h;
                    
                    // Il bit 0x400 marca il grassetto: va tolto prima di
                    // confrontare il tipo, altrimenti il testo bold non viene
                    // riconosciuto come tale e la sua altezza resta arbitraria.
                    if ((dest->type & 0xFF) == 3) {
                        el_w = get_prop_string_width(dest->text);
                        el_h = 16;
                    }
                    
                    int req_w = dest->x + el_w + 24; 
                    int req_h = dest->y + el_h + 40;
                    
                    // 1. LA LARGHEZZA: Espandiamo SOLO se NON c'è il flag e non è Testo
                    if (!ignore_w && (dest->type & 0xFF) != 3) {
                        if (req_w > windows[win_id].min_w) windows[win_id].min_w = req_w;
                    }
                    
                    // 2. L'ALTEZZA: Espandiamo SOLO se NON c'è il flag
                    if (!ignore_h) {
                        if (req_h > windows[win_id].min_h) windows[win_id].min_h = req_h;
                    }
                    
                    // 3. Applichiamo l'espansione visiva
                    if (windows[win_id].w < windows[win_id].min_w) windows[win_id].w = windows[win_id].min_w;
                    if (windows[win_id].h < windows[win_id].min_h) windows[win_id].h = windows[win_id].min_h;
                }
                refresh_requested = 1; 
            }
        }
    }

    // ========================================================
    // SYSCALL 8: LETTURA TASTIERA (Real-time)
    // ========================================================
    else if (sys_id == 8) {
        char* key_dest = (char*)arg1;

        // LA PATCH ANTI-FANTASMA: Ricevi il tasto SOLO se sei la finestra in primo piano
        // ECCEZIONE: Se sei al secondo posto (window_order[1]) ma sopra di te c'è il Menu Start,
        // sei comunque l'App attiva e ricevi la digitazione
        int is_top = 0;
        if (current_pid > 0) {
            int win_id = process_table[current_pid].linked_window;
            if (global_active_win == win_id) { // Usa il nuovo focus
                is_top = 1;
            } else if (global_active_win != -1 && windows[global_active_win].type == WIN_TYPE_START_MENU && window_order[1] == win_id) {
                is_top = 1; // Eccezione per il menu start aperto
            }
        }

        if (is_top) {
            *key_dest = last_key_pressed;
            last_key_pressed = 0; // Svuotiamo la buca delle lettere dopo averla letta
        } else {
            *key_dest = 0; // Se sei in background, il Kernel ti ignora e restituisce "nessun tasto" (0).
        }
    }

    // ========================================================
    // SYSCALL 9: INFO DIMENSIONI FINESTRA (Context-Aware)
    // ========================================================
    else if (sys_id == 9) {
        int* w_dest = (int*)arg1;
        int* h_dest = (int*)arg2;
        int target_win_id = (int)arg3; // Leggiamo il contesto dal registro EDX

        if (current_pid > 0) {
            int win_id = target_win_id;
            if (win_id <= 0) win_id = process_table[current_pid].linked_window;
            
            if (win_id >= 0 && win_id < MAX_WINDOWS && windows[win_id].owner_pid == current_pid) {
                *w_dest = windows[win_id].w; // Consegniamo la larghezza
                *h_dest = windows[win_id].h; // Consegniamo l'altezza
            }
        }
    }

    // ========================================================
    // SYSCALL 10: LETTURA FILE (Dal disco alla RAM App)
    // ========================================================
    else if (sys_id == 10) {
        process_table[current_pid].is_critical_io = 1; // ALZIAMO LO SCUDO
        // CODA DI ATTESA
        while (vfs_lock == 1) {
            asm volatile("sti \n pause \n cli"); 
        }
        uint32_t actual_size = 0;

        vfs_lock = 1; // Chiudiamo a nostra volta il lucchetto

        // Limite alzato a 300KB
        int success = ext2_read_file((const char*)arg1, (char*)arg2, 300000, &actual_size);
        
        if (success) {
            // Consegniamo il pacchetto con la grandezza esatta all'App
            if (arg3 != 0) *((uint32_t*)arg3) = actual_size;
        } else {
            char* err_buf = (char*)arg2;
            err_buf[0] = '\0';
            if (arg3 != 0) *((uint32_t*)arg3) = 0; // CORRETTO: Usiamo arg3 direttamente
        }
        vfs_lock = 0; // Apriamo il lucchetto
        process_table[current_pid].is_critical_io = 0; // ABBASSIAMO LO SCUDO
    }

    // ========================================================
    // SYSCALL 11: LEGGI TESTO DA ELEMENTO GUI
    // ========================================================
    else if (sys_id == 11) {
        int element_idx = arg1;
        char* dest_buffer = (char*)arg2;
        int target_win_id = (int)arg3; // NUOVO: Leggiamo l'ID della finestra

        if (current_pid > 0) {
            int win_id = target_win_id;
            if (win_id <= 0) win_id = process_table[current_pid].linked_window;
            
            // Verifichiamo il proprietario e l'indice
            if (win_id >= 0 && win_id < MAX_WINDOWS && windows[win_id].owner_pid == current_pid && element_idx >= 0 && element_idx < windows[win_id].ui_count) {
                
                // Peschiamo l'elemento grafico corretto dalla Display List
                gui_element_t* el = &windows[win_id].ui_elements[element_idx];
                
                // Copiamo il testo (max 15 caratteri) nel buffer dell'app
                int t = 0;
                while (el->text[t] != '\0' && t < 255) {
                    dest_buffer[t] = el->text[t];
                    t++;
                }
                dest_buffer[t] = '\0'; // Chiudiamo sempre la stringa in modo sicuro
                
            } else {
                // Errore: Indice non valido, restituiamo una stringa vuota per evitare crash
                dest_buffer[0] = '\0';
            }
        }
    }

    // ========================================================
    // SYSCALL 12: RIPRODUCI AUDIO WAA (48kHz Stereo)
    // ========================================================
    else if (sys_id == 12) {
        char* audio_path = (char*)arg1;
        
        // Se c'è un driver audio registrato e attivo, passagli la stringa
        if (sys_audio_play != 0) {
            sys_audio_play(audio_path);
        }
    }

    // ========================================================
    // SYSCALL 13: EXIT (Suicidio volontario dell'App)
    // ========================================================
    else if (sys_id == 13) {
        if (current_pid > 0) {
            // Finestre, memoria e taskbar: pensa a tutto close_app()
            kill_process();
        }
    }
    
    // ========================================================
    // SYSCALL 14: CREA FINESTRA FIGLIA (Multi-Window)
    // ========================================================
    else if (sys_id == 14) {
        char* title = (char*)arg1;
        // Spacchettiamo Larghezza e Altezza che l'App ci manderà compattati in un solo numero
        int req_w = (arg2 & 0xFFFF);
        int req_h = (arg2 >> 16) & 0xFFFF;
        int* out_win_id = (int*)arg3;
        
        if (current_pid > 0) {
            int parent_win = process_table[current_pid].linked_window;
            int start_x = 100; int start_y = 100;
            
            if (parent_win >= 0 && windows[parent_win].active) {
                start_x = windows[parent_win].x + 30; // Offset diagonale
                start_y = windows[parent_win].y + 30;
            }
            
            // Creiamo la finestra assegnandola al PROCESSO ATTUALE!
            int new_win = open_window(WIN_TYPE_APP_GUI, title, start_x, start_y, req_w, req_h, current_pid);
            
            // Consegniamo l'Handle (ID Finestra) all'app scrivendolo nel suo puntatore!
            if (out_win_id != 0) *out_win_id = new_win; 
            
            refresh_requested = 1;
        }
    }
    // ========================================================
    // SYSCALL 15: STATUS FINESTRA (E' ancora viva?)
    // ========================================================
    else if (sys_id == 15) {
        int win_id = (int)arg1;
        int* is_active = (int*)arg2;
        if (win_id >= 0 && win_id < MAX_WINDOWS && windows[win_id].owner_pid == current_pid) {
            *is_active = windows[win_id].active;
        } else {
            *is_active = 0;
        }
    }

    // ========================================================
    // SYSCALL 16: IMPOSTA DIMENSIONE INIZIALE/BASE FINESTRA
    // ========================================================
    else if (sys_id == 16) {
        int req_w = (int)arg1;
        int req_h = (int)arg2;
        int target_win_id = (int)arg3;
        
        if (current_pid > 0) {
            int win_id = target_win_id;
            if (win_id <= 0) win_id = process_table[current_pid].linked_window;
            
            if (win_id >= 0 && win_id < MAX_WINDOWS && windows[win_id].owner_pid == current_pid) {
                windows[win_id].base_w = req_w;
                windows[win_id].base_h = req_h;
                // Il minimo resta quello dello sviluppatore, se c'e': la misura
                // d'apertura non deve sovrascriverlo, qualunque sia l'ordine in cui
                // l'app chiama le due funzioni.
                if (windows[win_id].dev_min_w > 0) {
                    windows[win_id].min_w = windows[win_id].dev_min_w;
                    windows[win_id].min_h = windows[win_id].dev_min_h;
                } else {
                    windows[win_id].min_w = req_w;
                    windows[win_id].min_h = req_h;
                }
                windows[win_id].w = (req_w > windows[win_id].min_w) ? req_w : windows[win_id].min_w;
                windows[win_id].h = (req_h > windows[win_id].min_h) ? req_h : windows[win_id].min_h;
                
                // Rende visibile la finestra se l'app imposta subito la dimensione
                windows[win_id].is_initializing = 0; 
                
                // Ricalcoliamo istantaneamente la centratura
                windows[win_id].x = (fb_width - req_w) / 2;
                windows[win_id].y = (fb_height - 30 - req_h) / 2;
                if (windows[win_id].x < 0) windows[win_id].x = 0;
                if (windows[win_id].y < 0) windows[win_id].y = 0;
            }
            refresh_requested = 1;
        }
    }


    // --- SYSCALL 20 / 22: scrive / accoda testo in un'area multilinea ---
    // arg1 = indice dell'area, arg2 = testo, arg3 = finestra.
    // La 20 sostituisce tutto il contenuto, la 22 aggiunge in fondo e fa
    // scorrere la vista fino alla fine: e' quella adatta a una risposta che
    // arriva a pezzi, come quella di un modello linguistico.
    else if (sys_id == 20 || sys_id == 22) {
        int ti = (int)(arg1 & 0xFF);
        const char* src = (const char*)arg2;
        int target_win_id = (int)arg3;

        if (current_pid > 0 && src && ti < TA_MAX) {
            int win_id = target_win_id;
            if (win_id <= 0) win_id = process_table[current_pid].linked_window;

            if (win_id >= 0 && win_id < MAX_WINDOWS && windows[win_id].owner_pid == current_pid) {
                textarea_t* ta = &windows[win_id].ta[ti];
                if (!ta->buf) ta_alloc(ta);
                if (ta->buf) {
                    if (sys_id == 20) {
                        ta->len = 0;
                        ta->cursor = 0;
                        ta->scroll = 0;
                    }
                    ta_append_text(ta, src);    // UTF-8 dell'app -> Latin-1
                    ta->buf[ta->len] = '\0';
                    if (sys_id == 22) {
                        ta->cursor = ta->len;
                        ta->scroll = 1 << 20;   // il disegno la riporta all'ultima pagina
                    }
                    ta->version++;
                    refresh_requested = 1;
                }
            }
        }
    }

    // --- SYSCALL 21: legge il testo di un'area multilinea ---
    // arg1 = indice | (spazio disponibile << 8), arg2 = destinazione, arg3 = finestra
    else if (sys_id == 21) {
        int ti  = (int)(arg1 & 0xFF);
        int max = (int)(arg1 >> 8);
        char* dst = (char*)arg2;
        int target_win_id = (int)arg3;

        if (current_pid > 0 && dst && max > 0 && ti < TA_MAX) {
            int win_id = target_win_id;
            if (win_id <= 0) win_id = process_table[current_pid].linked_window;

            if (win_id >= 0 && win_id < MAX_WINDOWS && windows[win_id].owner_pid == current_pid) {
                textarea_t* ta = &windows[win_id].ta[ti];
                int n = 0;
                if (ta->buf)
                    for (; n < ta->len && n < max - 1; n++) dst[n] = ta->buf[n];
                dst[n] = '\0';
            }
        }
    }

    // --- SYSCALL 23: stato del robottino per questa finestra ---
    // arg1 = puntatore a int dove scrivere lo stato, arg3 = finestra
    else if (sys_id == 23) {
        int* out = (int*)arg1;
        int target_win_id = (int)arg3;

        robot_tick();   // la richiesta avanza mentre l'app osserva

        if (current_pid > 0 && out) {
            int win_id = target_win_id;
            if (win_id <= 0) win_id = process_table[current_pid].linked_window;

            if (win_id >= 0 && win_id < MAX_WINDOWS && windows[win_id].owner_pid == current_pid) {
                if (!robot_permesso(win_id))                       *out = ROBOT_ASSENTE;
                else if (robot_state == ROBOT_PENSA && robot_win == win_id) *out = ROBOT_PENSA;
                else if (robot_state == ROBOT_PENSA)               *out = ROBOT_ERRORE;
                else                                               *out = ROBOT_PRONTO;
            } else {
                *out = ROBOT_ASSENTE;
            }
        }
    }

    // --- SYSCALL 24: chiedi qualcosa all'intelligenza artificiale ---
    // arg1 = testo della domanda
    // arg2 = puntatore a int: in entrata l'indice dell'area in cui scrivere la
    //        risposta, in uscita 1 se la richiesta e' stata accettata
    // arg3 = finestra
    else if (sys_id == 24) {
        const char* prompt = (const char*)arg1;
        int* io = (int*)arg2;
        int target_win_id = (int)arg3;

        if (current_pid > 0 && io) {
            int area = *io;
            *io = 0;

            int win_id = target_win_id;
            if (win_id <= 0) win_id = process_table[current_pid].linked_window;

            if (prompt && area >= 0 && area < TA_MAX &&
                win_id >= 0 && win_id < MAX_WINDOWS &&
                windows[win_id].owner_pid == current_pid &&
                robot_permesso(win_id) && robot_state != ROBOT_PENSA) {

                int n = 0;
                while (prompt[n] && n < ROBOT_PROMPT_MAX) { robot_prompt[n] = prompt[n]; n++; }
                robot_prompt[n] = '\0';

                if (n > 0) {
                    robot_win = win_id;
                    robot_area = area;
                    robot_state = ROBOT_PENSA;
                    robot_t0 = system_ticks;
                    robot_phase = 1;
                    robot_parsed = 0;
                    robot_first_line = 1;
                    robot_build_request(robot_prompt);
                    tcp_connect(net_gw_ip, ROBOT_PORT);
                    *io = 1;
                    refresh_requested = 1;
                }
            }
        }
    }

    // --- SYSCALL 19: dimensione minima scelta dallo sviluppatore ---
    else if (sys_id == 19) {
        int req_w = (int)arg1;
        int req_h = (int)arg2;
        int target_win_id = (int)arg3;

        if (current_pid > 0) {
            int win_id = target_win_id;
            if (win_id <= 0) win_id = process_table[current_pid].linked_window;

            if (win_id >= 0 && win_id < MAX_WINDOWS && windows[win_id].owner_pid == current_pid) {
                // Pavimento di sistema: sotto questa misura la barra del titolo
                // non conterrebbe nemmeno i tre comandi della finestra.
                if (req_w < 120) req_w = 120;
                if (req_h < 60)  req_h = 60;

                windows[win_id].dev_min_w = req_w;
                windows[win_id].dev_min_h = req_h;
                windows[win_id].min_w = req_w;
                windows[win_id].min_h = req_h;

                // Se la finestra e' gia' piu' piccola del nuovo minimo, si allarga
                if (windows[win_id].w < req_w) windows[win_id].w = req_w;
                if (windows[win_id].h < req_h) windows[win_id].h = req_h;
                refresh_requested = 1;
            }
        }
    }

    // --- SYSCALL 17: dichiara un menu della barra superiore ---
    // L'app rimanda gli stessi menu a ogni fotogramma, sempre allo stesso
    // indice: cosi' non serve azzerarli, e la barra non sfarfalla nell'istante
    // fra la pulizia e la ridichiarazione.
    else if (sys_id == 17) {
        typedef struct { char title[24]; int item_count; char items[8][32]; } menu_pkt_t;
        menu_pkt_t* pkt = (menu_pkt_t*)arg1;
        int target_win_id = (int)arg2;
        int slot = (int)arg3;

        if (current_pid > 0 && pkt && slot >= 0 && slot < 6) {
            int win_id = target_win_id;
            if (win_id <= 0) win_id = process_table[current_pid].linked_window;

            if (win_id >= 0 && win_id < MAX_WINDOWS && windows[win_id].owner_pid == current_pid) {
                window_t* w = &windows[win_id];
                utf8_to_latin1(w->menu_titles[slot], 24, pkt->title, 24);

                int n = pkt->item_count;
                if (n < 0) n = 0;
                if (n > 8) n = 8;
                w->menu_item_count[slot] = n;
                for (int i = 0; i < n; i++) {
                    utf8_to_latin1(w->menu_items[slot][i], 32, pkt->items[i], 32);
                }
                if (slot + 1 > w->menu_count) w->menu_count = slot + 1;
            }
        }
    }

    // --- SYSCALL 18: quale voce di menu e' stata scelta ---
    else if (sys_id == 18) {
        int target_win_id = (int)arg1;
        int* out = (int*)arg2;

        if (current_pid > 0 && out) {
            *out = -1;
            int win_id = target_win_id;
            if (win_id <= 0) win_id = process_table[current_pid].linked_window;

            if (win_id >= 0 && win_id < MAX_WINDOWS && windows[win_id].owner_pid == current_pid) {
                if (windows[win_id].menu_action > 0) {
                    *out = windows[win_id].menu_action - 1;
                    windows[win_id].menu_action = 0;   // letta una volta sola
                }
            }
        }
    }

    else {
        print_term("ERRORE: Syscall sconosciuta chiamata dall'App.");
        render_desktop(mouse_x, mouse_y, is_clicking);
    }
    

}
// --- IL PORTIERE ASSEMBLY (Wrapper Syscall) ---
// Questa funzione intercetta l'hardware, prepara gli argomenti (arg3, arg2, arg1, sys_id)
// chiama il nostro codice C, e poi torna all'App chiudendo la porta in modo sicuro
__attribute__((naked)) void isr128_syscall() {
    asm volatile(
        "push %edx \n"            // arg3 
        "push %ecx \n"            // arg2
        "push %ebx \n"            // arg1
        "push %eax \n"            // sys_id
        "call syscall_handler \n" 
        "add $16, %esp \n"        // 4 parametri * 4 byte = Pulisce 16 byte
        "iret \n"                 
    );
}


// Compila una voce della rubrica
void idt_set_gate(uint8_t num, uint32_t base, uint16_t sel, uint8_t flags) {
    idt[num].base_low = (base & 0xFFFF);
    idt[num].base_high = (base >> 16) & 0xFFFF;
    idt[num].sel = sel;
    idt[num].zero = 0;
    idt[num].flags = flags;
}

// Inizializza la IDT
void init_idt() {
    idtp.limit = (sizeof(struct idt_entry) * 256) - 1;
    idtp.base = (uint32_t)&idt;

    uint16_t current_cs;
    asm volatile("mov %%cs, %0" : "=r"(current_cs));

    // 1. Azzeriamo tutti i 256 campanelli
    for (int i = 0; i < 256; i++) {
        idt_set_gate(i, 0, 0, 0);
    }

    // 2. Mappiamo le 32 Eccezioni Hardware sul nostro Kernel Panic Generico!
    for (int i = 0; i < 32; i++) {
        idt_set_gate(i, (uint32_t)isr_generic, current_cs, 0x8E);
    }

    // 3. Specializziamo le eccezioni più gravi coi nostri investigatori
    idt_set_gate(0, (uint32_t)isr0_divzero, current_cs, 0x8E);
    idt_set_gate(13, (uint32_t)isr13_gpf, current_cs, 0x8E);
    idt_set_gate(14, (uint32_t)isr14_pfault, current_cs, 0x8E);

    idt_set_gate(32, (uint32_t)isr32_timer, current_cs, 0x8E); // IL TIMER (IRQ0)

    idt_set_gate(44, (uint32_t)isr44_mouse, current_cs, 0x8E); // IL MOUSE (IRQ12)

    // Flags 0xEE significa: "Presente, Eseguibile anche dal Ring 3 (User Mode)"
    idt_set_gate(128, (uint32_t)isr128_syscall, current_cs, 0xEE);

    // 4. MAGIA: Carichiamo la rubrica nel cuore del processore x86
    asm volatile("lidt %0" : : "m" (idtp));
}

// ==========================================
// IL CUORE HARDWARE (PIC & PIT)
// ==========================================

// Riprogrammiamo il centralinista degli Interrupt (PIC)
void init_pic() {
    // 1. Inizializzazione (ICW1)
    outb(0x20, 0x11); outb(0xA0, 0x11);
    
    // 2. Remapping (ICW2): Spostiamo gli interrupt hardware dal numero 32 in poi
    // (Perché da 0 a 31 sono riservati per gli errori della CPU come il "Divided by Zero")
    outb(0x21, 0x20); // Master PIC parte da 32 (0x20)
    outb(0xA1, 0x28); // Slave PIC parte da 40 (0x28)
    
    // 3. Cascading (ICW3): Diciamo come sono collegati tra loro
    outb(0x21, 0x04); outb(0xA1, 0x02);
    
    // 4. Modalità (ICW4): Modalità x86 standard
    outb(0x21, 0x01); outb(0xA1, 0x01);
    
    // 5. MASCHERE (FONDAMENTALE): 
    // Blocchiamo tutti gli interrupt hardware TRANNE il Timer (Bit 0 del Master).
    // In questo modo tastiera e mouse continueranno a funzionare col nostro vecchio metodo!
    // Master: timer (bit 0) e cascata verso lo slave (bit 2), da cui passa il mouse.
    outb(0x21, 0xFA); // 11111010
    // Slave: solo il mouse, IRQ12 (bit 4). La tastiera resta in polling.
    outb(0xA1, 0xEF); // 11101111
}

// Accendiamo il Metronomo (PIT)
void init_pit(uint32_t frequency) {
    // La frequenza base del chip è 1193180 Hz. La dividiamo per ottenere la nostra!
    uint32_t divisor = 1193180 / frequency;
    
    // Inviamo il comando: Canale 0, Modalità "Onda Quadra"
    outb(0x43, 0x36); 
    
    // Inviamo il divisore (prima la parte bassa, poi la parte alta)
    outb(0x40, (uint8_t)(divisor & 0xFF));
    outb(0x40, (uint8_t)((divisor >> 8) & 0xFF));
}


// ==========================================
// MOTORE DEL TEMPO (SLEEP)
// ==========================================
void sleep(uint32_t ms) {
    // Controlliamo se gli interrupt sono abilitati leggendo il registro EFLAGS della CPU
    uint32_t eflags;
    asm volatile("pushf \n pop %0" : "=r"(eflags));

    if (eflags & 0x0200) { 
        // GLI INTERRUPT SONO ACCESI! Usiamo l'orologio hardware esatto (PIT)
        uint32_t ticks_to_wait = ms / 10; // 100 Hz = 1 tick ogni 10 ms
        if (ticks_to_wait == 0) ticks_to_wait = 1;
        uint32_t start_ticks = system_ticks;
        
        while (system_ticks - start_ticks < ticks_to_wait) {
            // Manda a dormire la CPU fino al prossimo "tick" per non surriscaldare il PC!
            asm volatile("hlt"); 
        }
    } else {
        // GLI INTERRUPT SONO SPENTI: Fallback di emergenza
        for(volatile uint32_t d = 0; d < (ms * 50000); d++);
    }
}

// ==========================================
// MOTORE GRAFICO FASE DI BOOT
// ==========================================
int boot_cursor_y = 20;

// Stampa un messaggio bianco
void print_boot(const char* msg) {
    draw_string(msg, 10, boot_cursor_y, 0xFFFFFFFF, 1); // Scale 1 (Normale)
    boot_cursor_y += 14;                                // Interlinea ridotta per Scale 1
    swap_buffers(); 
    
    // RITARDO TRIPLICATO per una lettura umana
    for(volatile int d = 0; d < 50000000; d++); 
}

// Stampa un successo [ OK ] in verde
void print_boot_ok(const char* msg) {
    draw_string("[ OK ]", 10, boot_cursor_y, 0xFF00FF00, 1); // Scale 1
    draw_string(msg, 80, boot_cursor_y, 0xFFCCCCCC, 1);      // Scale 1
    boot_cursor_y += 14;
    swap_buffers();
    
    // RITARDO TRIPLICATO!
    for(volatile int d = 0; d < 30000000; d++); 
}

// Stampa un errore [FAIL] in rosso
void print_boot_err(const char* msg) {
    draw_string("[FAIL]", 10, boot_cursor_y, 0xFFFF0000, 1); // Scale 1
    draw_string(msg, 80, boot_cursor_y, 0xFFFF0000, 1);      // Scale 1
    boot_cursor_y += 14;
    swap_buffers();
    
    for(volatile int d = 0; d < 80000000; d++); 
}


// ============================================================================
// RETE: comandi di prova
// ============================================================================
static void net_hex2(char* s, uint8_t v) {
    const char* h = "0123456789ABCDEF";
    s[0] = h[v >> 4];
    s[1] = h[v & 0xF];
}

// Scrive un MAC come "52:54:00:12:34:56" in s, restituisce la nuova lunghezza
static int net_put_mac(char* s, int k, const uint8_t* m) {
    for (int i = 0; i < 6; i++) {
        net_hex2(&s[k], m[i]);
        k += 2;
        if (i < 5) s[k++] = ':';
    }
    return k;
}

void net_report(void) {
    if (!net_drv) { print_term("Rete: nessun driver caricato."); return; }

    uint8_t m[6];
    net_drv->get_mac(m);

    char s[64];
    int k = 0;
    const char* a = "Rete: RTL8139, MAC ";
    while (*a) s[k++] = *a++;
    k = net_put_mac(s, k, m);
    a = ", IRQ ";
    while (*a) s[k++] = *a++;
    if (net_drv->irq >= 10) s[k++] = (char)('0' + net_drv->irq / 10);
    s[k++] = (char)('0' + net_drv->irq % 10);
    s[k] = '\0';
    print_term(s);
}

// Il primo pacchetto verso il mondo esterno: una richiesta ARP in broadcast,
// "chi ha l'indirizzo 10.0.2.2?". Nella rete virtuale di QEMU risponde il Mac.
void net_arping(void) {
    if (!net_drv) { print_term("Rete: nessun driver caricato."); return; }

    uint8_t m[6];
    net_drv->get_mac(m);

    uint8_t f[42];
    for (int i = 0; i < 6; i++) { f[i] = 0xFF; f[6 + i] = m[i]; }   // a tutti, da noi
    f[12] = 0x08; f[13] = 0x06;                                      // protocollo: ARP
    f[14] = 0x00; f[15] = 0x01;                                      // rete Ethernet
    f[16] = 0x08; f[17] = 0x00;                                      // indirizzi IPv4
    f[18] = 6;    f[19] = 4;                                         // lunghezze MAC e IP
    f[20] = 0x00; f[21] = 0x01;                                      // e' una richiesta
    for (int i = 0; i < 6; i++) f[22 + i] = m[i];                    // il nostro MAC
    f[28] = 10; f[29] = 0; f[30] = 2; f[31] = 15;                    // il nostro IP
    for (int i = 0; i < 6; i++) f[32 + i] = 0;                       // MAC cercato: ignoto
    f[38] = 10; f[39] = 0; f[40] = 2; f[41] = 2;                     // IP cercato

    if (!net_drv->send(f, sizeof(f))) { print_term("ARP: invio fallito."); return; }
    print_term("ARP: chi ha 10.0.2.2? Richiesta inviata.");

    // Attesa della risposta, al massimo due secondi
    static uint8_t buf[1600];
    uint32_t start = system_ticks;
    while (system_ticks - start < 200) {
        int n = net_drv->receive(buf, sizeof(buf));
        if (n >= 42 && buf[12] == 0x08 && buf[13] == 0x06 &&          // ARP
            buf[20] == 0x00 && buf[21] == 0x02 &&                     // risposta
            buf[28] == 10 && buf[29] == 0 && buf[30] == 2 && buf[31] == 2) {
            char s[64];
            int k = 0;
            const char* a = "ARP: 10.0.2.2 risponde da ";
            while (*a) s[k++] = *a++;
            k = net_put_mac(s, k, &buf[22]);
            s[k] = '\0';
            print_term(s);
            return;
        }
    }
    print_term("ARP: nessuna risposta entro 2 secondi.");
}


// ============================================================================
// PILA DI RETE: Ethernet, ARP, IPv4, ICMP
// Nella rete virtuale di QEMU gli indirizzi sono fissi e noti, quindi per ora
// non serve il DHCP: noi siamo 10.0.2.15 e il Mac, visto da qui, e' 10.0.2.2.
// Tutto e' non bloccante: net_poll() viene chiamata dal ciclo principale e
// smaltisce i pacchetti arrivati senza mai fermare l'interfaccia.
// I numeri nei pacchetti di rete vanno scritti col byte piu' significativo per
// primo, al contrario di come li tiene l'x86: per questo si compongono a mano
// byte per byte invece di copiarci sopra una struttura.
// ============================================================================
static const uint8_t net_my_ip[4] = { 10, 0, 2, 15 };
static const uint8_t net_gw_ip[4] = { 10, 0, 2, 2  };
static uint8_t net_my_mac[6];
static int     net_mac_ready = 0;

#define ARP_CACHE 4
static struct {
    uint8_t ip[4];
    uint8_t mac[6];
    int     valid;
} arp_cache[ARP_CACHE];

static uint8_t  net_tx[1600];      // un frame in uscita per volta
static uint16_t net_ip_id = 1;

// Ultimo ping in attesa di risposta
static uint16_t ping_id = 0x3D00, ping_seq = 0;
static int      ping_ok = 0;
static uint32_t ping_t0 = 0, ping_ticks = 0;

static int net_ip_eq(const uint8_t* a, const uint8_t* b) {
    return a[0] == b[0] && a[1] == b[1] && a[2] == b[2] && a[3] == b[3];
}

static void net_my_mac_load(void) {
    if (!net_mac_ready && net_drv) {
        net_drv->get_mac(net_my_mac);
        net_mac_ready = 1;
    }
}

// Somma di controllo dei protocolli internet: somma a 16 bit con i riporti
// ripiegati sopra, poi invertita.
static uint16_t net_checksum(const uint8_t* p, int len) {
    uint32_t s = 0;
    int i = 0;
    for (; i + 1 < len; i += 2) s += (uint32_t)((p[i] << 8) | p[i + 1]);
    if (i < len) s += (uint32_t)(p[i] << 8);
    while (s >> 16) s = (s & 0xFFFF) + (s >> 16);
    return (uint16_t)(~s);
}

static void arp_remember(const uint8_t* ip, const uint8_t* mac) {
    int free_slot = -1;
    for (int i = 0; i < ARP_CACHE; i++) {
        if (arp_cache[i].valid && net_ip_eq(arp_cache[i].ip, ip)) {
            for (int k = 0; k < 6; k++) arp_cache[i].mac[k] = mac[k];
            return;
        }
        if (!arp_cache[i].valid && free_slot < 0) free_slot = i;
    }
    if (free_slot < 0) free_slot = 0;   // nessuno libero: si ricicla il primo
    for (int k = 0; k < 4; k++) arp_cache[free_slot].ip[k] = ip[k];
    for (int k = 0; k < 6; k++) arp_cache[free_slot].mac[k] = mac[k];
    arp_cache[free_slot].valid = 1;
}

static const uint8_t* arp_lookup(const uint8_t* ip) {
    for (int i = 0; i < ARP_CACHE; i++)
        if (arp_cache[i].valid && net_ip_eq(arp_cache[i].ip, ip)) return arp_cache[i].mac;
    return 0;
}

// oper 1 = domanda "chi ha questo indirizzo?", 2 = risposta "ce l'ho io"
static void net_send_arp(int oper, const uint8_t* target_ip, const uint8_t* target_mac) {
    if (!net_drv) return;
    net_my_mac_load();

    uint8_t f[42];
    for (int i = 0; i < 6; i++) {
        f[i]     = target_mac ? target_mac[i] : 0xFF;   // senza destinatario: a tutti
        f[6 + i] = net_my_mac[i];
    }
    f[12] = 0x08; f[13] = 0x06;
    f[14] = 0x00; f[15] = 0x01;          // rete Ethernet
    f[16] = 0x08; f[17] = 0x00;          // indirizzi IPv4
    f[18] = 6;    f[19] = 4;
    f[20] = 0x00; f[21] = (uint8_t)oper;
    for (int i = 0; i < 6; i++) f[22 + i] = net_my_mac[i];
    for (int i = 0; i < 4; i++) f[28 + i] = net_my_ip[i];
    for (int i = 0; i < 6; i++) f[32 + i] = target_mac ? target_mac[i] : 0;
    for (int i = 0; i < 4; i++) f[38 + i] = target_ip[i];

    net_drv->send(f, sizeof(f));
}

// Spedisce un pacchetto IP. Se non sappiamo ancora a quale scheda corrisponde
// l'indirizzo, manda la domanda ARP e restituisce 0: chi chiama riprovera'.
static int net_send_ip(const uint8_t* dst_ip, uint8_t proto, const uint8_t* payload, int plen) {
    if (!net_drv || plen < 0 || plen > 1400) return 0;
    net_my_mac_load();

    const uint8_t* dmac = arp_lookup(dst_ip);
    if (!dmac) {
        net_send_arp(1, dst_ip, 0);
        return 0;
    }

    uint8_t* f = net_tx;
    for (int i = 0; i < 6; i++) { f[i] = dmac[i]; f[6 + i] = net_my_mac[i]; }
    f[12] = 0x08; f[13] = 0x00;

    uint8_t* ip = f + 14;
    int total = 20 + plen;
    ip[0] = 0x45;                        // versione 4, intestazione di 20 byte
    ip[1] = 0x00;
    ip[2] = (uint8_t)(total >> 8); ip[3] = (uint8_t)total;
    ip[4] = (uint8_t)(net_ip_id >> 8); ip[5] = (uint8_t)net_ip_id;
    net_ip_id++;
    ip[6] = 0x40; ip[7] = 0x00;          // non frammentare
    ip[8] = 64;                          // vite residue del pacchetto
    ip[9] = proto;
    ip[10] = 0; ip[11] = 0;
    for (int i = 0; i < 4; i++) { ip[12 + i] = net_my_ip[i]; ip[16 + i] = dst_ip[i]; }

    uint16_t ck = net_checksum(ip, 20);
    ip[10] = (uint8_t)(ck >> 8); ip[11] = (uint8_t)ck;

    for (int i = 0; i < plen; i++) ip[20 + i] = payload[i];
    return net_drv->send(f, 14 + total);
}

// ============================================================================
// TCP: client minimale, una connessione per volta
// Basta per parlare con un server HTTP sulla stessa macchina. Non c'e'
// controllo di congestione ne' riordino dei segmenti fuori sequenza: un
// segmento inatteso viene scartato e si rimanda la conferma precedente, cosi'
// l'altro capo lo ritrasmette. Sulla rete virtuale di QEMU, che non perde e
// non riordina, e' piu' che sufficiente; su Internet non basterebbe.
// ============================================================================
#define TH_FIN 0x01
#define TH_SYN 0x02
#define TH_RST 0x04
#define TH_PSH 0x08
#define TH_ACK 0x10

#define TCP_CHIUSA   0
#define TCP_ATTESA   1   // SYN inviato, aspettiamo la risposta
#define TCP_APERTA   2   // connessione stabilita
#define TCP_FINITA   3   // l'altro capo ha chiuso: i dati sono tutti qui
#define TCP_ERRORE   4

#define TCP_MSS      1400
#define TCP_TX_MAX   4096
#define TCP_RX_MAX   16384
#define TCP_RTO      50    // tick prima di ritrasmettere (mezzo secondo)
#define TCP_RETRIES  8

static struct {
    int      state;
    uint8_t  peer_ip[4];
    uint16_t local_port, peer_port;
    uint32_t snd_una;      // primo byte inviato e non ancora confermato
    uint32_t snd_nxt;      // prossima sequenza da usare
    uint32_t data_seq0;    // sequenza del primo byte di dati
    uint32_t rcv_nxt;      // prossimo byte che ci aspettiamo
    uint8_t  tx[TCP_TX_MAX];
    int      tx_len, tx_sent;
    uint32_t t_last;
    int      retries;
} tcp;

static uint8_t  tcp_rx[TCP_RX_MAX];
static int      tcp_rx_len = 0;

// Somma a 16 bit accumulabile su piu' pezzi: serve perche' la somma di
// controllo del TCP copre anche uno pseudo-intestazione che non viene spedita.
static uint32_t net_sum16(uint32_t s, const uint8_t* p, int len) {
    int i = 0;
    for (; i + 1 < len; i += 2) s += (uint32_t)((p[i] << 8) | p[i + 1]);
    if (i < len) s += (uint32_t)(p[i] << 8);
    return s;
}

static uint16_t net_fold(uint32_t s) {
    while (s >> 16) s = (s & 0xFFFF) + (s >> 16);
    return (uint16_t)(~s);
}

static uint32_t be32(const uint8_t* p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

static void put32(uint8_t* p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);  p[3] = (uint8_t)v;
}

// Confronto fra sequenze: contano le differenze, non i valori assoluti,
// perche' i numeri di sequenza girano attorno allo zero.
static int seq_gt(uint32_t a, uint32_t b) { return (int32_t)(a - b) > 0; }

static void tcp_out(uint8_t flags, const uint8_t* data, int dlen) {
    // static: lo stack del kernel e' di 4 KB, e questo buffer da solo ne
    // occuperebbe un terzo abbondante. Non serve che sia rientrante.
    static uint8_t seg[20 + 4 + TCP_MSS];
    int opt = 0;

    if (flags & TH_SYN) {          // nel SYN si dichiara la misura massima accettata
        seg[20] = 2; seg[21] = 4;
        seg[22] = (uint8_t)(TCP_MSS >> 8); seg[23] = (uint8_t)TCP_MSS;
        opt = 4;
    }
    int hdr = 20 + opt;

    seg[0] = (uint8_t)(tcp.local_port >> 8); seg[1] = (uint8_t)tcp.local_port;
    seg[2] = (uint8_t)(tcp.peer_port >> 8);  seg[3] = (uint8_t)tcp.peer_port;
    put32(&seg[4], tcp.snd_nxt);
    put32(&seg[8], (flags & TH_ACK) ? tcp.rcv_nxt : 0);
    seg[12] = (uint8_t)((hdr / 4) << 4);
    seg[13] = flags;

    int win = TCP_RX_MAX - tcp_rx_len;       // quanto spazio ci resta
    if (win < 0) win = 0;
    if (win > 32768) win = 32768;
    seg[14] = (uint8_t)(win >> 8); seg[15] = (uint8_t)win;
    seg[16] = 0; seg[17] = 0;                // somma di controllo, dopo
    seg[18] = 0; seg[19] = 0;

    for (int i = 0; i < dlen; i++) seg[hdr + i] = data[i];
    int total = hdr + dlen;

    // Pseudo-intestazione: indirizzi, protocollo e lunghezza. Non viaggia sulla
    // rete, ma entra nel conto: e' cosi' che il TCP si accorge se un pacchetto
    // e' arrivato all'indirizzo sbagliato.
    uint8_t ph[12];
    for (int i = 0; i < 4; i++) { ph[i] = net_my_ip[i]; ph[4 + i] = tcp.peer_ip[i]; }
    ph[8] = 0; ph[9] = 6;
    ph[10] = (uint8_t)(total >> 8); ph[11] = (uint8_t)total;

    uint16_t ck = net_fold(net_sum16(net_sum16(0, ph, 12), seg, total));
    seg[16] = (uint8_t)(ck >> 8); seg[17] = (uint8_t)ck;

    net_send_ip(tcp.peer_ip, 6, seg, total);
}

// Manda i dati non ancora spediti, a pezzi non piu' grandi di un segmento
static void tcp_push(void) {
    while (tcp.tx_sent < tcp.tx_len) {
        int n = tcp.tx_len - tcp.tx_sent;
        if (n > TCP_MSS) n = TCP_MSS;
        tcp.snd_nxt = tcp.data_seq0 + (uint32_t)tcp.tx_sent;
        tcp_out(TH_ACK | TH_PSH, &tcp.tx[tcp.tx_sent], n);
        tcp.tx_sent += n;
        tcp.snd_nxt = tcp.data_seq0 + (uint32_t)tcp.tx_sent;
    }
    tcp.t_last = system_ticks;
}

int tcp_connect(const uint8_t* ip, uint16_t port) {
    if (!net_drv) return 0;
    for (int i = 0; i < 4; i++) tcp.peer_ip[i] = ip[i];
    tcp.peer_port  = port;
    tcp.local_port = (uint16_t)(40000 + (system_ticks & 0x3FFF));
    tcp.snd_nxt    = (system_ticks * 2654435761u) | 1;   // sequenza iniziale
    tcp.snd_una    = tcp.snd_nxt;
    tcp.rcv_nxt    = 0;
    tcp.tx_len = 0; tcp.tx_sent = 0;
    tcp.retries = 0;
    tcp_rx_len = 0;
    tcp.state = TCP_ATTESA;

    tcp_out(TH_SYN, 0, 0);
    tcp.snd_nxt++;                     // il SYN occupa una sequenza
    tcp.t_last = system_ticks;
    return 1;
}

// Mette i dati in coda. Vengono spediti appena la connessione e' aperta.
int tcp_send(const char* data, int len) {
    if (tcp.state != TCP_ATTESA && tcp.state != TCP_APERTA) return 0;
    if (len > TCP_TX_MAX) return 0;
    for (int i = 0; i < len; i++) tcp.tx[i] = (uint8_t)data[i];
    tcp.tx_len = len;
    tcp.tx_sent = 0;
    if (tcp.state == TCP_APERTA) {
        tcp.data_seq0 = tcp.snd_nxt;
        tcp_push();
    }
    return 1;
}

static void net_rx_tcp(const uint8_t* t, int len, const uint8_t* src_ip) {
    if (len < 20 || tcp.state == TCP_CHIUSA) return;

    uint16_t sport = (uint16_t)((t[0] << 8) | t[1]);
    uint16_t dport = (uint16_t)((t[2] << 8) | t[3]);
    if (dport != tcp.local_port || sport != tcp.peer_port || !net_ip_eq(src_ip, tcp.peer_ip)) return;

    uint32_t seq = be32(&t[4]);
    uint32_t ack = be32(&t[8]);
    int hdr = (t[12] >> 4) * 4;
    uint8_t fl = t[13];
    if (hdr < 20 || hdr > len) return;

    const uint8_t* data = t + hdr;
    int dlen = len - hdr;

    if (fl & TH_RST) { tcp.state = TCP_ERRORE; return; }

    // Stretta di mano: al SYN+ACK rispondiamo con la conferma e partiamo
    if (tcp.state == TCP_ATTESA) {
        if ((fl & (TH_SYN | TH_ACK)) == (TH_SYN | TH_ACK) && ack == tcp.snd_nxt) {
            tcp.rcv_nxt = seq + 1;
            tcp.snd_una = ack;
            tcp.state   = TCP_APERTA;
            tcp_out(TH_ACK, 0, 0);
            if (tcp.tx_len > 0 && tcp.tx_sent == 0) {
                tcp.data_seq0 = tcp.snd_nxt;
                tcp_push();
            }
        }
        return;
    }

    if ((fl & TH_ACK) && seq_gt(ack, tcp.snd_una)) {
        tcp.snd_una = ack;
        tcp.retries = 0;
        tcp.t_last = system_ticks;
    }

    if (dlen > 0) {
        if (seq == tcp.rcv_nxt) {
            for (int i = 0; i < dlen && tcp_rx_len < TCP_RX_MAX; i++)
                tcp_rx[tcp_rx_len++] = data[i];
            tcp.rcv_nxt += (uint32_t)dlen;
        }
        // In sequenza o no, si conferma: se no, la conferma ripetuta fa
        // ritrasmettere all'altro capo il pezzo che ci manca.
        tcp_out(TH_ACK, 0, 0);
    }

    if ((fl & TH_FIN) && seq + (uint32_t)dlen == tcp.rcv_nxt) {
        tcp.rcv_nxt++;                    // anche il FIN occupa una sequenza
        tcp_out(TH_ACK | TH_FIN, 0, 0);
        tcp.snd_nxt++;
        tcp.state = TCP_FINITA;
    }
}

// Ritrasmissione: se dopo mezzo secondo non e' arrivata conferma, si rimanda.
static void tcp_tick(void) {
    if (tcp.state != TCP_ATTESA && tcp.state != TCP_APERTA) return;

    // Se tutto quello che abbiamo mandato e' gia' stato confermato non c'e'
    // niente da ritrasmettere: stiamo solo aspettando che parli l'altro capo,
    // e puo' metterci minuti. Contare tentativi qui farebbe cadere la
    // connessione mentre il modello sta ancora pensando.
    if (tcp.state == TCP_APERTA && !seq_gt(tcp.snd_nxt, tcp.snd_una)) {
        tcp.t_last = system_ticks;
        tcp.retries = 0;
        return;
    }

    if (system_ticks - tcp.t_last < TCP_RTO) return;

    if (tcp.retries >= TCP_RETRIES) { tcp.state = TCP_ERRORE; return; }
    tcp.retries++;
    tcp.t_last = system_ticks;

    if (tcp.state == TCP_ATTESA) {
        tcp.snd_nxt = tcp.snd_una;
        tcp_out(TH_SYN, 0, 0);
        tcp.snd_nxt++;
    } else if (seq_gt(tcp.snd_nxt, tcp.snd_una)) {
        // Si riparte dal primo byte non confermato
        tcp.tx_sent = (int)(tcp.snd_una - tcp.data_seq0);
        if (tcp.tx_sent < 0 || tcp.tx_sent > tcp.tx_len) tcp.tx_sent = 0;
        tcp_push();
    }
}
// Butta via i primi n byte gia' letti: senza questo il buffer di ricezione
// si riempirebbe e le risposte lunghe verrebbero troncate.
void tcp_rx_consume(int n) {
    if (n <= 0) return;
    if (n > tcp_rx_len) n = tcp_rx_len;
    for (int i = n; i < tcp_rx_len; i++) tcp_rx[i - n] = tcp_rx[i];
    tcp_rx_len -= n;
}
int tcp_state(void)   { return tcp.state; }
int tcp_rx_size(void) { return tcp_rx_len; }
const char* tcp_rx_data(void) { return (const char*)tcp_rx; }

static void net_rx_arp(const uint8_t* f, int n) {
    if (n < 42) return;
    int oper = (f[20] << 8) | f[21];
    const uint8_t* sender_mac = &f[22];
    const uint8_t* sender_ip  = &f[28];
    const uint8_t* target_ip  = &f[38];

    arp_remember(sender_ip, sender_mac);          // vale sia per domande sia per risposte

    if (oper == 1 && net_ip_eq(target_ip, net_my_ip))
        net_send_arp(2, sender_ip, sender_mac);   // cercano noi: rispondiamo
}

static void net_rx_icmp(const uint8_t* ip, int iplen, const uint8_t* src_ip) {
    if (iplen < 8) return;
    uint8_t type = ip[0];

    if (type == 8) {                              // ci stanno pingando: rispondiamo
        static uint8_t rep[1400];
        int n = iplen < 1400 ? iplen : 1400;
        for (int i = 0; i < n; i++) rep[i] = ip[i];
        rep[0] = 0;                               // tipo: risposta
        rep[2] = 0; rep[3] = 0;
        uint16_t ck = net_checksum(rep, n);
        rep[2] = (uint8_t)(ck >> 8); rep[3] = (uint8_t)ck;
        net_send_ip(src_ip, 1, rep, n);
    }
    else if (type == 0) {                         // e' la risposta al nostro ping
        uint16_t id = (uint16_t)((ip[4] << 8) | ip[5]);
        if (id == ping_id) {
            ping_ok = 1;
            ping_ticks = system_ticks - ping_t0;
        }
    }
}

static void net_rx_ip(const uint8_t* f, int n) {
    const uint8_t* ip = f + 14;
    if (n < 34 || (ip[0] >> 4) != 4) return;

    int ihl = (ip[0] & 0x0F) * 4;
    int total = (ip[2] << 8) | ip[3];
    if (ihl < 20 || total < ihl || 14 + total > n) return;
    if (!net_ip_eq(&ip[16], net_my_ip)) return;   // non e' per noi

    const uint8_t* payload = ip + ihl;
    int plen = total - ihl;

    if (ip[9] == 1)      net_rx_icmp(payload, plen, &ip[12]);
    else if (ip[9] == 6) net_rx_tcp(payload, plen, &ip[12]);
}

// Smaltisce i pacchetti arrivati. Il tetto di sedici per giro evita che una
// raffica di traffico faccia aspettare la grafica.
void net_poll(void) {
    if (!net_drv) return;
    tcp_tick();                 // ritrasmissioni e scadenze
    static uint8_t buf[1600];
    for (int guard = 0; guard < 16; guard++) {
        int n = net_drv->receive(buf, sizeof(buf));
        if (n <= 0) return;
        if (n < 14) continue;
        uint16_t et = (uint16_t)((buf[12] << 8) | buf[13]);
        if (et == 0x0806)      net_rx_arp(buf, n);
        else if (et == 0x0800) net_rx_ip(buf, n);
    }
}


// Prova della pila completa: una richiesta HTTP a Ollama sul Mac.
// "Connection: close" chiede al server di chiudere appena ha finito: cosi'
// sappiamo che la risposta e' completa senza doverne interpretare la lunghezza.
void net_http_test(void) {
    if (!net_drv) { print_term("Rete: nessun driver caricato."); return; }

    if (!tcp_connect(net_gw_ip, 11434)) { print_term("http: connessione non avviata."); return; }
    print_term("http: connessione a 10.0.2.2:11434 ...");

    uint32_t t0 = system_ticks;
    while (system_ticks - t0 < 300 && tcp_state() == TCP_ATTESA) { net_poll(); }

    if (tcp_state() != TCP_APERTA) {
        print_term("http: nessuna risposta (Ollama e' in esecuzione sul Mac?)");
        return;
    }

    // La lunghezza si conta, non si scrive a mano: un byte in piu' o in meno
    // manderebbe spazzatura in coda alla richiesta.
    const char* req = "GET / HTTP/1.1\r\nHost: 10.0.2.2:11434\r\nConnection: close\r\n\r\n";
    int rl = 0; while (req[rl]) rl++;
    tcp_send(req, rl);

    t0 = system_ticks;
    while (system_ticks - t0 < 1000 && tcp_state() == TCP_APERTA) { net_poll(); }

    int n = tcp_rx_size();
    if (n <= 0) { print_term("http: nessun dato ricevuto."); return; }

    // Stampa la risposta riga per riga, saltando i ritorni a capo
    const char* d = tcp_rx_data();
    char line[128];
    int k = 0;
    for (int i = 0; i < n; i++) {
        char c = d[i];
        if (c == '\n' || k >= 120) {
            line[k] = '\0';
            print_term(line);
            k = 0;
            if (c != '\n') i--;
        } else if (c != '\r') {
            line[k++] = c;
        }
    }
    if (k > 0) { line[k] = '\0'; print_term(line); }
}

// Comando ping: manda una domanda e aspetta la risposta, con un tetto di due
// secondi. E' l'unico punto bloccante, ed e' accettabile perche' lo chiede
// l'utente dal terminale.
void net_ping(void) {
    if (!net_drv) { print_term("Rete: nessun driver caricato."); return; }
    net_my_mac_load();

    // Prima serve l'indirizzo fisico del destinatario
    if (!arp_lookup(net_gw_ip)) {
        net_send_arp(1, net_gw_ip, 0);
        uint32_t t = system_ticks;
        while (system_ticks - t < 100 && !arp_lookup(net_gw_ip)) net_poll();
        if (!arp_lookup(net_gw_ip)) { print_term("ping: 10.0.2.2 non risponde all'ARP."); return; }
    }

    uint8_t echo[40];
    ping_seq++;
    echo[0] = 8; echo[1] = 0;                     // domanda di eco
    echo[2] = 0; echo[3] = 0;
    echo[4] = (uint8_t)(ping_id >> 8); echo[5] = (uint8_t)ping_id;
    echo[6] = (uint8_t)(ping_seq >> 8); echo[7] = (uint8_t)ping_seq;
    for (int i = 8; i < 40; i++) echo[i] = (uint8_t)('a' + (i % 23));
    uint16_t ck = net_checksum(echo, 40);
    echo[2] = (uint8_t)(ck >> 8); echo[3] = (uint8_t)ck;

    ping_ok = 0;
    ping_t0 = system_ticks;
    if (!net_send_ip(net_gw_ip, 1, echo, 40)) { print_term("ping: invio fallito."); return; }

    while (system_ticks - ping_t0 < 200 && !ping_ok) net_poll();

    if (!ping_ok) { print_term("ping: nessuna risposta da 10.0.2.2."); return; }

    char s[64];
    int k = 0;
    const char* a = "ping: risposta da 10.0.2.2 in ";
    while (*a) s[k++] = *a++;
    uint32_t ms = ping_ticks * 10;                // il timer batte ogni 10 ms
    if (ms == 0) {
        const char* b = "meno di 10 ms";
        while (*b) s[k++] = *b++;
    } else {
        char d[12];
        int j = 0;
        do { d[j++] = (char)('0' + ms % 10); ms /= 10; } while (ms && j < 10);
        while (j) s[k++] = d[--j];
        s[k++] = ' '; s[k++] = 'm'; s[k++] = 's';
    }
    s[k] = '\0';
    print_term(s);
}

// ==========================================
// KERNEL (MAIN)
// ==========================================

// ============================================================================
// MENU PRINCIPALE: lettura delle app, avvio, tastiera, click, spegnimento
// ============================================================================
// Legge da /apps i file .edxi e li mette in ordine alfabetico nel menu.
// E' lo stesso codice che prima usava il sottomenu Applications.
static void start_menu_load_apps(window_t* win) {
    win->t_len = 0; // Usiamo t_len per contare le app
    uint8_t sector_buffer[1024] = {0};

    // 3. LETTURA DISCO ALLA RICERCA DEGLI .EDXI NELLA CARTELLA /apps
    uint32_t apps_inode_num = ext2_resolve_path("/apps");
    if (apps_inode_num > 0) {
        ext2_inode_t apps_inode = ext2_get_inode_struct(apps_inode_num);
        if ((apps_inode.i_mode & 0xF000) == 0x4000) { // È davvero una cartella?
            
            
            // Leggiamo i dati della directory /apps
            if (ata_read_sector(apps_inode.i_block[0] * 2, sector_buffer) && 
                ata_read_sector((apps_inode.i_block[0] * 2) + 1, sector_buffer + 512)) {
                
                uint32_t offset = 0;
                while (offset < 1024) { 
                    ext2_dir_entry_t entry; uint8_t* edst = (uint8_t*)&entry;
                    for(size_t j = 0; j < sizeof(ext2_dir_entry_t); j++) edst[j] = sector_buffer[offset + j];
                    if (entry.inode == 0 || entry.rec_len == 0) break;

                    char filename[256];
                    for (int k = 0; k < entry.name_len; k++) filename[k] = sector_buffer[offset + 8 + k];
                    
                    // Filtriamo solo i file .edxi
                    int len = entry.name_len;
                    if (len >= 5 && filename[len-5] == '.' && filename[len-4] == 'e' && filename[len-3] == 'd' && filename[len-2] == 'x' && filename[len-1] == 'i') {
                        filename[len-5] = '\0';
                        int f=0;
                        while(filename[f] && f < 149) {
                            win->t_history[win->t_len][f] = filename[f];
                            f++;
                        }
                        win->t_history[win->t_len][f] = '\0';
                        win->t_len++;
                        if (win->t_len >= TERM_MAX_ROWS) break; 
                    }
                    offset += entry.rec_len;
                }
            }
        }
    }

    // 4. ORDINAMENTO ALFABETICO (Bubble Sort Integrato e Formattato)
    int count = win->t_len;
    for (int m = 0; m < count - 1; m++) {
        for (int n = 0; n < count - m - 1; n++) {
            int cmp = 0; int c = 0;
            while(win->t_history[n][c] && win->t_history[n+1][c]) {
                if (win->t_history[n][c] > win->t_history[n+1][c]) { cmp = 1; break; }
                if (win->t_history[n][c] < win->t_history[n+1][c]) { cmp = -1; break; }
                c++;
            }
            if (cmp == 0 && win->t_history[n][c] != '\0' && win->t_history[n+1][c] == '\0') cmp = 1;

            if (cmp > 0) { // Scambio testuale
                char temp[150]; 
                int k = 0;
                
                while((temp[k] = win->t_history[n][k]) != '\0') {
                    k++;
                }
                temp[k] = '\0';
                
                k = 0; 
                while((win->t_history[n][k] = win->t_history[n+1][k]) != '\0') {
                    k++;
                }
                win->t_history[n][k] = '\0';
                
                k = 0; 
                while((win->t_history[n+1][k] = temp[k]) != '\0') {
                    k++;
                }
                win->t_history[n+1][k] = '\0';
            }
        }
    }
}

// Avvia un'applicazione dal suo file .edxi: la legge, le prepara la memoria,
// apre la sua finestra e crea il processo. Prima questo codice stava dentro
// il gestore dei click del sottomenu; ora lo puo' usare chiunque.
static void launch_app(const char* target_file) {
    // 3. IL LANCIO DELL'APP (Ora con Demand Paging)
    uint32_t* app_pt = (uint32_t*)kmalloc(4096);

    if (app_pt) {
        // Puliamo la tabella
        for(int k=0; k<1024; k++) app_pt[k] = 0;

        uint32_t actual_size = 0;
        void* temp_buffer = kmalloc(500000); // Buffer temporaneo per leggere il file
        
        if (temp_buffer && ext2_read_file(target_file, (char*)temp_buffer, 500000, &actual_size)) {
            
            // Quanti frame servono per l'eseguibile?
            uint32_t frames_needed = (actual_size / 4096) + 1;
            
            // Allochiamo i frame fisici
            for(uint32_t f = 0; f < frames_needed; f++) {
                uint32_t phys_frame = pfa_alloc_frame();
                app_pt[f] = phys_frame | 0x07; 
                
                uint8_t* dest = (uint8_t*)phys_frame; 
                uint8_t* src = (uint8_t*)temp_buffer + (f * 4096);
                for(int b = 0; b < 4096; b++) {
                    if ((f * 4096 + b) < actual_size) {
                        dest[b] = src[b];
                    } else {
                        dest[b] = 0; // AZZERIAMO LA CODA DEL FRAME
                    }
                }
            }
            
            kfree(temp_buffer); // Finito, liberiamo la RAM temporanea del Kernel

            uint8_t* mem_ptr = (uint8_t*)(app_pt[0] & 0xFFFFF000); 

            int is_gui = 0;
            int is_virtual = 0; 
            uint32_t entry_offset = 0;
            
            // Controllo Header EDXI
            if (mem_ptr[0] == 'E' && mem_ptr[1] == 'D' && mem_ptr[2] == 'X' && mem_ptr[3] == 'I') {
                if (mem_ptr[4] == 1) is_gui = 1;
                if (mem_ptr[5] == 1) is_virtual = 1; 
                entry_offset = 16; 
            } else {
                print_term("ERRORE: File .edxi corrotto.");
                for(uint32_t f = 0; f < frames_needed; f++) pfa_free_frame(app_pt[f] & 0xFFFFF000);
                kfree(app_pt);
                return;
            }
            
            if (!is_virtual) {
                print_term("SICUREZZA: App obsoleta (Ring 0). Esecuzione bloccata!");
                for(uint32_t f = 0; f < frames_needed; f++) pfa_free_frame(app_pt[f] & 0xFFFFF000);
                kfree(app_pt);
                return;
            }
            
            // Offset fisso Virtuale
            void (*app_func)(void) = (void (*)(void))(0x40000000 + entry_offset);
            
            int new_pid = get_free_pid();
            if (new_pid != -1) {
                if (is_gui) {
                    int app_win_id = open_window(WIN_TYPE_APP_GUI, target_file, 150, 150, 400, 300, new_pid);
                    // FIX: Le app lanciate dal Menu non hanno finestre genitore! (ID = -1)
                    create_process(app_func, app_win_id, app_pt, is_virtual, -1, new_pid); 
                } else {
                    int app_win_id = open_window(WIN_TYPE_TERMINAL, target_file, 200, 200, 500, 400, new_pid);
                    create_process(app_func, app_win_id, app_pt, is_virtual, -1, new_pid);
                }
            } else {
                print_term("ERRORE: Raggiunto il limite massimo di processi attivi.");
                for(uint32_t f = 0; f < frames_needed; f++) pfa_free_frame(app_pt[f] & 0xFFFFF000);
                kfree(app_pt);
            }
        } else {
            print_term("Errore: Impossibile leggere il file.");
            if (temp_buffer) kfree(temp_buffer);
            kfree(app_pt);
        }
    } else {
        print_term("Errore: Impossibile creare la Page Table!");
    }
}

// Spegnimento. Su QEMU, Bochs e VirtualBox basta scrivere su una porta ACPI
// nota. Su un PC vero servirebbe leggere le tabelle ACPI della macchina, che
// il sistema ancora non sa fare: li' si mostra la classica schermata e si
// ferma la CPU, e l'utente spegne con il pulsante.
static void system_shutdown(void) {
    outw(0x604, 0x2000);    // QEMU, macchine recenti
    outw(0xB004, 0x2000);   // Bochs e QEMU piu' vecchi
    outw(0x4004, 0x3400);   // VirtualBox

    // Se siamo ancora qui, la macchina non si e' spenta da sola
    for (int i = 0; i < fb_width * fb_height; i++) backbuffer[i] = 0xFF000000;
    const char* msg = "Ora puoi spegnere il computer.";
    int tw = get_prop_string_width_bold(msg);
    draw_prop_string_bold(msg, (fb_width - tw) / 2, fb_height / 2 - 8, 0xFFF5A623);
    swap_buffers();
    asm volatile("cli");
    while (1) asm volatile("hlt");
}

// Il menu principale aperto, se c'e'
static int start_menu_find(void) {
    for (int m = 0; m < MAX_WINDOWS; m++)
        if (windows[m].active && windows[m].type == WIN_TYPE_START_MENU) return m;
    return -1;
}

static void start_menu_close(void) {
    for (int m = 0; m < MAX_WINDOWS; m++)
        if (windows[m].type == WIN_TYPE_START_MENU || windows[m].type == WIN_TYPE_APP_MENU)
            windows[m].active = 0;
    sm_exit_armed_until = 0;
    sm_press = SM_PRESS_NONE;
    refresh_requested = 1;
}

// Chiude il menu e avvia l'app numero idx dell'elenco
static void start_menu_launch(window_t* win, int idx) {
    char target_file[64];
    int f = 0;
    const char* prefix = "/apps/";
    while (prefix[f]) { target_file[f] = prefix[f]; f++; }
    for (int k = 0; win->t_history[idx][k] && f < 58; k++) target_file[f++] = win->t_history[idx][k];
    target_file[f++] = '.'; target_file[f++] = 'e'; target_file[f++] = 'd';
    target_file[f++] = 'x'; target_file[f++] = 'i';
    target_file[f] = '\0';

    start_menu_close();
    launch_app(target_file);
}

// Tastiera, mentre il menu e' aperto: si scrive nella ricerca, Invio avvia
// la prima app trovata, Esc chiude il menu
static void start_menu_key(int sm, uint8_t sc) {
    window_t* win = &windows[sm];
    if (sc == 0x01) { start_menu_close(); return; }              // Esc

    char c = scancode_to_ascii[sc];
    int n = 0;
    while (win->t_buffer[n]) n++;

    if (c == '\b') {
        if (n > 0) win->t_buffer[n - 1] = '\0';
        win->t_scroll = 0;
    } else if (c == '\n') {
        if (sm_filter(win) > 0) { start_menu_launch(win, sm_hits[0]); return; }
    } else if (c >= 32 && c <= 126 && n < SM_QUERY_MAX) {
        win->t_buffer[n] = c;
        win->t_buffer[n + 1] = '\0';
        win->t_scroll = 0;
    }
    blink_counter = 0;
    refresh_requested = 1;
}

// Pressione dentro il menu. La barra di scorrimento risponde subito; per tutto
// il resto ci si limita a ricordare cosa e' stato premuto, e il disegno lo
// mostra scurito. L'azione vera avviene al rilascio, in start_menu_release().
static void start_menu_click(int sm, int mouse_x, int mouse_y) {
    window_t* win = &windows[sm];
    int x = win->x, y = win->y;
    int lx = x + SM_LIST_X;

    int n = sm_filter(win);
    int has_sb = (n > SM_ROWS);
    int list_top = y + SM_ROW_Y, list_bot = list_top + SM_ROWS * SM_ROW_H;

    if (has_sb && mouse_x >= lx + SM_LIST_W - 16 && mouse_x < lx + SM_LIST_W - 4 &&
        mouse_y >= list_top && mouse_y < list_bot) {
        // Barra di scorrimento: sopra la meta' sale di una pagina, sotto scende
        win->t_scroll += (mouse_y < (list_top + list_bot) / 2) ? -SM_ROWS : SM_ROWS;
        refresh_requested = 1;          // i limiti li rimette a posto il disegno
        return;
    }

    sm_press = sm_target_at(win, mouse_x, mouse_y);
    if (sm_press != SM_PRESS_NONE) refresh_requested = 1;
}

// Rilascio del tasto del mouse. La voce premuta si apre solo se il puntatore
// e' ancora sopra di lei: chi si accorge di aver sbagliato puo' spostarsi
// fuori prima di lasciare il tasto, e non succede nulla.
static void start_menu_release(int mouse_x, int mouse_y) {
    if (sm_press == SM_PRESS_NONE) return;
    int pressed = sm_press;
    sm_press = SM_PRESS_NONE;
    refresh_requested = 1;              // la voce torna chiara

    int sm = start_menu_find();
    if (sm == -1) return;
    window_t* win = &windows[sm];
    if (sm_target_at(win, mouse_x, mouse_y) != pressed) return;

    if (pressed >= 0) {
        start_menu_launch(win, pressed);
    } else if (pressed == SM_PRESS_TERMINAL) {
        start_menu_close();
        open_window(WIN_TYPE_TERMINAL, "Terminal", 150, 100, 500, 400, 0);
        print_term("New session started...");
    } else if (pressed == SM_PRESS_EXIT) {
        // Il primo rilascio chiede conferma per tre secondi, il secondo spegne
        if (system_ticks < sm_exit_armed_until) system_shutdown();
        sm_exit_armed_until = system_ticks + 300;
    }
}


void kernel_main(void) {
    

    // 1. Accendiamo SOLO la scheda video
    init_graphics();
    
    // 2. Dipingiamo lo schermo di Azzurro per la Boot Screen Grafica
    draw_bootscreen_background();
    bootscreen_status("Avvio III-Millennium...");

    // --- FASE 1: MEMORIA ---
    init_paging();
    init_heap();
    init_dma_heap();

    // Specchio per le dirty rectangles (3 MB dall'heap, non dalla zona DMA)
    shadow_buffer = (uint32_t*)kmalloc(fb_width * fb_height * sizeof(uint32_t));

    uint32_t detected_ram = detect_system_ram();
    init_pfa(detected_ram);
    bootscreen_status("Gestione Memoria e Paging... [OK]");

    // --- FASE 2: CORE KERNEL ---
    init_gdt();
    init_idt();
    bootscreen_status("Architettura x86 Protetta... [OK]");

    // --- FASE 3: HARDWARE E CONTROLLER ---
    mouse_init();
    init_pic();
    init_pit(100);

    
    // =======================================================
    // ACCENSIONE PRECOCE DEL MOTORE DEL TEMPO
    // Registriamo il Kernel nello Scheduler per evitare crash, 
    // e accendiamo gli interrupt hardware fin da ora
    // =======================================================
    process_table[0].active = 1;
    process_table[0].linked_window = -1;
    asm volatile("sti"); 

    
    bootscreen_status("Inizializzazione Periferiche I/O... [OK]");

    // --- FASE 4: DISCO FISSO E FILE SYSTEM ---
    if (ata_identify()) {
        bootscreen_status("Rilevamento disco fisso IDE/ATA... [OK]");

        if (ext2_mount()) {
            // IL FILE SYSTEM E' MONTATO! ORA POSSIAMO CARICARE IL LOGO
            if (load_bootscreen_logo()) {
                draw_bootscreen_background(); // Ridisegna lo sfondo ma con il logo al centro
            }
            bootscreen_status("Montaggio File System Ext2... [OK]");
        } else {
            bootscreen_status("ERRORE: Superblocco Ext2 non trovato!");
        }
    } else {
        bootscreen_status("ERRORE: Nessun Disco ATA Primario rilevato!");
    }


    // Pausa di 2 secondi esatti
    sleep(2000);
    
    bootscreen_status("Preparazione Interfaccia Grafica...");
    
    // 5 secondi esatti di attesa
    sleep(5000);


    // PULIZIA RAM KERNEL: Il logo ha fatto il suo lavoro, svuotiamolo per non intasare l'Heap
    if (bootscreen_logo_raw) {
        kfree(bootscreen_logo_raw);
        bootscreen_logo_raw = 0;
    }


    // ORA SIAMO PRONTI A LANCIARE IL DESKTOP
    init_window_manager();


    // Prepariamo il "Dizionario" per i Moduli esterni
    init_driver_api();

    

    // Calcoliamo la posizione iniziale del mouse ORA che il sistema è sveglio
    mouse_x = fb_width / 2;
    mouse_y = fb_height / 2;

    render_desktop(mouse_x, mouse_y, is_clicking);

    uint8_t mouse_cycle = 0;
    int8_t mouse_byte[3];

    // Inizializza lo storico del terminale vuoto
    for(int i=0; i<TERM_MAX_ROWS; i++) term_history[i][0] = '\0';


    print_term("Inizializzazione Sistema Operativo completata.");
    // --- CHECK HARDWARE DISCO ALL'AVVIO ---
    if (ata_identify()) {
        char boot_msg[100];
        int idx = 0;
        
        char* lbl = "HDA Rilevato: ";
        while(*lbl) boot_msg[idx++] = *lbl++;
        char* mod = ata_drive_model;
        while(*mod) boot_msg[idx++] = *mod++;
        char* lbl2 = " (";
        while(*lbl2) boot_msg[idx++] = *lbl2++;
        char num_mb[32]; itoa(ata_drive_capacity_mb, num_mb);
        char* n = num_mb; while(*n) boot_msg[idx++] = *n++;
        char* lbl3 = " MB)";
        while(*lbl3) boot_msg[idx++] = *lbl3++;
        boot_msg[idx] = '\0';
        
        print_term(boot_msg);

        // ========================================================
        // MOUNT DEL FILE SYSTEM EXT2 (IL NUOVO PASSO)
        // ========================================================
        if (ext2_mount()) {
            char mount_msg[100];
            idx = 0;
            char* m_lbl1 = "Ext2 Montato: "; while(*m_lbl1) mount_msg[idx++] = *m_lbl1++;
            char num_bg[32]; itoa(ext2_total_groups, num_bg);
            char* n_bg = num_bg; while(*n_bg) mount_msg[idx++] = *n_bg++;
            char* m_lbl2 = " Block Groups attivi."; while(*m_lbl2) mount_msg[idx++] = *m_lbl2++;
            mount_msg[idx] = '\0';
            print_term(mount_msg);
        } else {
            print_term("ERRORE CRITICO: Impossibile montare il file system Ext2!");
        }

    } else {
        print_term("ATTENZIONE: Nessun Disco ATA Primario rilevato!");
    }
    // NUOVI NOMI DELLE VARIABILI PER NON ANDARE IN CONFLITTO CON IL BOOT
    char term_ram_msg[64] = "RAM Fisica installata: ";
    char term_ram_num[16]; itoa(detected_ram, term_ram_num);
    int t_r_idx = 23; int t_rn = 0;
    while(term_ram_num[t_rn]) term_ram_msg[t_r_idx++] = term_ram_num[t_rn++];
    term_ram_msg[t_r_idx++] = ' '; term_ram_msg[t_r_idx++] = 'M'; term_ram_msg[t_r_idx++] = 'B'; term_ram_msg[t_r_idx] = '\0';
    print_term(term_ram_msg);
    /*
    print_term("                                              "); //questo è un bel ricordo, dove tutto è iniziato
    print_term("                                              ");
    print_term("====     Ante-Millennium OS x86 v0.1      ====");
    print_term("                                              ");
    print_term("-    DISCLAIMER: versione sperimentale!      -");
    print_term("                                              ");
    print_term("--------- Canale YouTube 511break ------------");
    print_term("                                              ");
    print_term("Digita 'help' e premi Invio per la lista comandi.");
    */
    // AUTOMAZIONE AVVIO AUDIO
    print_term("Caricamento demone audio AC97...");
    load_driver("/system/drivers/ac97.drv");
    load_driver("/system/drivers/rtl8139.drv");

    // Tema del sistema: si applica da solo a ogni avvio. Il comando theme del
    // terminale resta disponibile per cambiarlo al volo.
    if (!load_theme("/system/themes/sea.json"))
        print_term("ATTENZIONE: tema /system/themes/sea.json non trovato.");
    
    if (sys_audio_play != 0) {
        sys_audio_play("/system/media/startup.wav");
    } else {
        print_term("ATTENZIONE: Driver audio non caricato.");
    }
    refresh_requested = 1;

    int shift_pressed = 0; // 0 = Rilasciato, 1 = Premuto

    
    // Prima del ciclo, dichiariamo la memoria del tempo
    uint8_t last_second = 0; 
    uint32_t last_render_tick = 0; 
    
    while (1) {
        // V-SYNC STRICT: 50 FPS massimi! (100 tick / 2)
        if (refresh_requested && (system_ticks - last_render_tick >= 2 || anim_active)) {
            refresh_requested = 0;
            last_render_tick = system_ticks; 
            render_desktop(mouse_x, mouse_y, is_clicking);
        }

        // La rete lavora fra un fotogramma e l'altro: risponde all'ARP e al ping
        // anche quando nessuna applicazione la sta usando.
        net_poll();


        // Anche la risposta dell'IA avanza qui. L'app che l'ha chiesta dorme
        // (gui_yield la sospende fino al primo evento del mouse), ma il testo
        // vive nel kernel e lo disegna il kernel: non serve svegliarla per
        // vederlo comparire.
        robot_tick();

        // Il mouse arriva per interrupt: i suoi byte stanno nel buffer ad anello.
        // La tastiera resta in polling, ma si legge la porta SOLO se il byte
        // presente e' suo (bit 5 a zero): se e' del mouse, e' dell'interrupt.
        uint8_t ps2_status = inb(0x64);
        int mouse_ready = (mouse_ring_tail != mouse_ring_head);
        int kbd_ready   = (ps2_status & 0x01) && !(ps2_status & 0x20);

        if (mouse_ready || kbd_ready) {
            if (mouse_ready) {
                mouse_byte[mouse_cycle++] = (int8_t)mouse_ring[mouse_ring_tail];
                mouse_ring_tail = (uint8_t)(mouse_ring_tail + 1);

                // RISINCRONIZZAZIONE: nel primo byte di ogni pacchetto il bit 3 vale
                // sempre 1. Se durante un render lungo il buffer PS/2 perde un byte,
                // senza questo controllo la sequenza resta sfasata per sempre e la
                // freccia impazzisce.
                if (mouse_cycle == 1 && !((uint8_t)mouse_byte[0] & 0x08)) {
                    mouse_cycle = 0;   // byte spurio: scartato, restiamo allineati
                }

                if (mouse_cycle == 3) {
                    mouse_cycle = 0;

                    // GLI SPOSTAMENTI SONO NUMERI A 9 BIT: gli 8 bit bassi stanno nei
                    // byte 1 e 2, il SEGNO nei bit 4 (X) e 5 (Y) del primo byte.
                    // Leggerli come semplici int8_t funziona solo finche' il mouse si
                    // sposta di meno di 128 unita' per pacchetto: nei movimenti veloci
                    // il valore cambiava segno e la freccia saltava all'indietro,
                    // tanto che sembrava sparire.
                    uint8_t mflags = (uint8_t)mouse_byte[0];
                    int dx = (uint8_t)mouse_byte[1];
                    int dy = (uint8_t)mouse_byte[2];
                    if (mflags & 0x10) dx -= 256;
                    if (mflags & 0x20) dy -= 256;

                    // OVERFLOW (bit 6/7): il movimento era piu' ampio di quanto stia
                    // nel pacchetto. Invece di azzerarlo, e fermare la freccia, si
                    // prende il massimo nella direzione giusta.
                    if (mflags & 0x40) dx = (mflags & 0x10) ? -255 : 255;
                    if (mflags & 0x80) dy = (mflags & 0x20) ? -255 : 255;

                    prev_clicking = is_clicking;


                    is_clicking = mouse_byte[0] & 0x01;
                    
                    mouse_x += dx;
                    mouse_y -= dy;

                    if (mouse_x < 0) mouse_x = 0;
                    if (mouse_x >= fb_width) mouse_x = fb_width - 1;
                    if (mouse_y < 0) mouse_y = 0;
                    if (mouse_y >= fb_height) mouse_y = fb_height - 1;
                    
                    // =======================================================
                    // IL RIDISEGNO FLUIDO DEL CURSORE
                    // Se il mouse si muove o clicca, ridisegniamo il cursore!
                    // Ora che abbiamo bloccato il risveglio multiplo delle app,
                    // il motore grafico è abbastanza veloce da reggerlo a 100 FPS.
                    // =======================================================
                    if (is_clicking != prev_clicking) {
                        refresh_requested = 1;   // lo stato "premuto" cambia la grafica
                    }
                    else if (dx != 0 || dy != 0) {
                        if (mascot_drag || dragged_window != -1 || resizing_window != -1 || scrolling_window != -1) {
                            refresh_requested = 1;          // la scena si muove davvero
                        } else {
                            int sig = compute_hover_sig();
                            if (sig != last_hover_sig) {
                                last_hover_sig = sig;
                                refresh_requested = 1;      // l'evidenziazione è cambiata
                            } else {
                                cursor_move(mouse_x, mouse_y); // solo la freccia
                            }
                        }
                    }

                    // =======================================================
                    // OTTIMIZZAZIONE 2: LA SVEGLIA SPIETATA (MULTI-WINDOW)
                    // =======================================================
                    int active_win = window_order[0];
                    for(int p=1; p<MAX_PROCESSES; p++) {
                        if (process_table[p].active && process_table[p].waiting_for_input == 2) {
                            // FIX 4: Se la finestra attiva APPARTIENE a questo processo, sveglialo
                            if (windows[active_win].owner_pid == p) {
                                process_table[p].waiting_for_input = 0; 
                            }
                        }
                    }

                    


                   
                    

                    // --- LOGICA DI INTERAZIONE MOUSE (Z-Ordered) ---
                    if (is_clicking == 1 && prev_clicking == 0) {
                        
                        // Calcolo dinamico della Y per le hitbox animate
                        int tb_y = taskbar_content_y();

                        // PRESA DELLA MASCOTTE: prima di tutto il resto, cosi'
                        // afferrarla non porta in primo piano la finestra sotto
                        // ne' fa scattare il trascinamento della barra del titolo,
                        // e soprattutto non azzera il fuoco qui sotto.
                        {
                            int mxp, myp;
                            mascot_pos(&mxp, &myp);
                            if (mascot_hit(mouse_x, mouse_y)) {
                                mascot_drag = 1;
                                mascot_grab_dx = mouse_x - mxp;
                                mascot_grab_dy = mouse_y - myp;
                                refresh_requested = 1;
                                prev_clicking = is_clicking;
                                continue;
                            }
                        }
                        // VASSOIO DI CASA VUOTO: cliccandolo, una nuvoletta dice
                        // quale applicazione sta usando la mascotte.
                        if (mascot_where() != 1 && mascot_host >= 0 &&
                            // Vassoio piu' la colonna in cui salgono le particelle:
                            // nascono 4px sopra la base, salgono di 20 e hanno un
                            // raggio di 3, quindi arrivano a circa 27px d'altezza.
                            mouse_x >= home_tray_cx - 8 && mouse_x <= home_tray_cx + 8 &&
                            mouse_y >= home_tray_base - 27 && mouse_y <= home_tray_base) {
                            mascot_tip = !mascot_tip;
                            refresh_requested = 1;
                            prev_clicking = is_clicking;
                            continue;
                        }
                        mascot_tip = 0;   // qualunque altro click la richiude
                        // DI DEFAULT TOGLIAMO IL FOCUS A TUTTI (Se il click cade nel vuoto resterà così)
                        int old_tooltip = active_tooltip_window; // Salviamo la nuvoletta aperta prima di resettarla
                        active_tooltip_window = -1;
                        
                        // SVUOTIAMO IL BUFFER TASTIERA AL CAMBIO DI FOCUS
                        last_key_pressed = 0;

                        // ========================================================================
                        // HITBOX TASKBAR DINAMICA E CHIUSURA MENU
                        // ========================================================================
                        int tb_w = taskbar_w;
                        int tb_x = (fb_width - tb_w) / 2;
                        
                        int clicked_on_menu = 0;
                        // Il pulsante Start ora segue l'ancoraggio sinistro dinamico (tb_x + 7)
                        int start_btn_x = tb_x + 7;
                        int clicked_on_start_btn = (mouse_x >= start_btn_x && mouse_x <= start_btn_x + 80 && mouse_y >= tb_y && mouse_y <= tb_y + 26);

                        // 1. Verifichiamo se il click è caduto dentro un menu aperto
                        for (int m = 0; m < MAX_WINDOWS; m++) {
                            if (windows[m].active && (windows[m].type == WIN_TYPE_START_MENU || windows[m].type == WIN_TYPE_APP_MENU)) {
                                if (mouse_x >= windows[m].x && mouse_x <= windows[m].x + windows[m].w && 
                                    mouse_y >= windows[m].y && mouse_y <= windows[m].y + windows[m].h) {
                                    clicked_on_menu = 1; break;
                                }
                            }
                        }

                        // 2. Se clicco nel vuoto chiudi i menu
                        if (!clicked_on_menu && !clicked_on_start_btn) {
                            for (int m = 0; m < MAX_WINDOWS; m++) {
                                if (windows[m].active && (windows[m].type == WIN_TYPE_START_MENU || windows[m].type == WIN_TYPE_APP_MENU)) {
                                    windows[m].active = 0;
                                    refresh_requested = 1;
                                }
                            }
                        }

                        if (clicked_on_start_btn) {
                            int menu_open = -1;
                            for(int m=0; m<MAX_WINDOWS; m++) {
                                if(windows[m].active && windows[m].type == WIN_TYPE_START_MENU) menu_open = m;
                            }

                            if (menu_open != -1) {
                                windows[menu_open].active = 0; 
                                for(int m=0; m<MAX_WINDOWS; m++) if(windows[m].type == WIN_TYPE_APP_MENU) windows[m].active = 0; 
                            } else {
                                // Il menu principale si apre allineato al pulsante Main,
                                // qualche pixel sopra la barra. L'elenco delle app si
                                // rilegge a ogni apertura, cosi' e' sempre aggiornato.
                                int sm = open_window(WIN_TYPE_START_MENU, "", start_btn_x,
                                                     taskbar_panel_top() - 8 - SM_H, SM_W, SM_H, 0);
                                if (sm != -1) {
                                    windows[sm].t_buffer[0] = '\0';
                                    windows[sm].t_scroll = 0;
                                    start_menu_load_apps(&windows[sm]);
                                }
                                sm_bg_ok = 0;              // lo strato fisso va ridisegnato
                                sm_exit_armed_until = 0;
                            }
                            refresh_requested = 1;
                            continue; 
                        }

                        // ========================================================================
                        // CLICK SULL'ICONA "INFO" (Ancorata a destra) - ARMA IL CLICK
                        // ========================================================================
                        int clock_x_hit = (tb_x + tb_w) - 33;
                        int info_w_hit = 26;
                        int info_x_hitbox = clock_x_hit - info_w_hit - 10; 
                        
                        if (mouse_x >= info_x_hitbox && mouse_x <= info_x_hitbox + info_w_hit && mouse_y >= tb_y && mouse_y <= tb_y + 26) {
                            pressed_info_btn = 1; // Armiamo il bottone
                            continue; 
                        }
                        
                        // ========================================================================
                        // CLICK SUI PULSANTI DELLE APP (Area centrale dinamica e Sincronizzata)
                        // ========================================================================
                        int tb_start_x_hit = start_btn_x + 88; // Inizio esatto: tb_x + 95
                        
                        // CORREZIONE BUG: Allineiamo la fine della hitbox al calcolo visivo del separatore integrale
                        int tb_end_x_hit = (info_x_hitbox - 15) - 12; 
                        
                        if (mouse_y >= tb_y && mouse_y <= tb_y + 26 && mouse_x >= tb_start_x_hit && mouse_x <= tb_end_x_hit) {
                            int valid_wins[MAX_WINDOWS];
                            int valid_count = get_taskbar_windows(valid_wins);
                            if (valid_count > 0) {
                                int tb_area_w = tb_end_x_hit - tb_start_x_hit;
                                
                                // Usiamo la stessa soglia di tolleranza visiva (> 10)
                                if (tb_area_w > 10) {
                                    int btn_w = tb_area_w / valid_count;
                                    if (btn_w > 160) btn_w = 160;
                                    
                                    // I bottoni sono cliccabili SOLO se sono visibili (stessa soglia grafica btn_w > 30)
                                    if (btn_w > 30) {
                                        int clicked_idx = (mouse_x - tb_start_x_hit) / btn_w;
                                        
                                        // Precisione Pixel-Perfect: calcoliamo in che punto del singolo slot è caduto il mouse.
                                        // Poiché il bottone visivo è largo (btn_w - 4), escludiamo i 4px di gap a destra!
                                        int local_click_x = (mouse_x - tb_start_x_hit) % btn_w;
                                        
                                        if (clicked_idx >= 0 && clicked_idx < valid_count && local_click_x <= (btn_w - 4)) {
                                            int target_win = valid_wins[clicked_idx];
                                            bring_to_front(target_win);
                                            windows[target_win].is_minimized = 0;
                                            global_active_win = target_win; 
                                            
                                            update_taskbar_state(); 
                                            
                                            for(int m=0; m<MAX_WINDOWS; m++) {
                                                if (windows[m].active && (windows[m].type == WIN_TYPE_START_MENU || windows[m].type == WIN_TYPE_APP_MENU)) windows[m].active = 0;
                                            }
                                            refresh_requested = 1;
                                            continue; 
                                        }
                                    }
                                }
                            }
                        }


                        // ====================================================
                        // MENU APERTO: la tendina intercetta il click prima di
                        // chiunque altro, altrimenti finirebbe alla finestra
                        // che si trova sotto di essa.
                        // ====================================================
                        if (open_menu_win >= 0 && open_menu_idx >= 0 &&
                            windows[open_menu_win].active) {
                            window_t* omw = &windows[open_menu_win];
                            int n  = omw->menu_item_count[open_menu_idx];
                            int dx = menu_title_box(omw, open_menu_idx, 0);
                            int dw = menu_drop_w(omw, open_menu_idx);
                            int dy = PANEL_BAR_H;

                            if (mouse_x > dx && mouse_x < dx + dw - 1 &&
                                mouse_y >= dy + 3 && mouse_y < dy + 3 + n * MENU_ITEM_H) {
                                int item = (mouse_y - (dy + 3)) / MENU_ITEM_H;
                                omw->menu_action = open_menu_idx * 100 + item + 1;
                            }
                            open_menu_win = -1;
                            open_menu_idx = -1;
                            refresh_requested = 1;
                            continue;          // in ogni caso il click e' consumato
                        }


                        // SCUDO TASKBAR: Ferma i click diretti alle finestre sotto la barra (Scudo animato)
                        if (mouse_y >= tb_y) {
                            continue; 
                        }

                        // ====================================================
                        // SCUDO PANNELLO: la barra dei menu appartiene alla
                        // finestra che ha il fuoco, non a quella che si trova
                        // geometricamente sotto di essa. Cliccarla equivale
                        // quindi a cliccare la finestra gia' selezionata, e il
                        // fuoco non deve cambiare. Senza questo scudo il test
                        // di appartenenza piu' sotto pescherebbe la finestra
                        // massimizzata, che parte sopra il pannello.
                        // Se nessuna finestra ha il fuoco, la barra e' del
                        // desktop e il click viene semplicemente assorbito.
                        // ====================================================
                        if (mouse_y < PANEL_BAR_H) {
                            // Titoli dei menu: aprono e chiudono la tendina
                            int mo = panel_owner();
                            if (mo >= 0 && windows[mo].menu_count > 0) {
                                for (int mi = 0; mi < windows[mo].menu_count; mi++) {
                                    int mw, mx0 = menu_title_box(&windows[mo], mi, &mw);
                                    if (mouse_x >= mx0 && mouse_x < mx0 + mw) {
                                        if (open_menu_win == mo && open_menu_idx == mi) {
                                            open_menu_win = -1; open_menu_idx = -1;
                                        } else {
                                            open_menu_win = mo; open_menu_idx = mi;
                                        }
                                        refresh_requested = 1;
                                        break;
                                    }
                                }
                            }
                            int f = global_active_win;
                            if (f >= 0 && f < MAX_WINDOWS && windows[f].active && !windows[f].is_minimized) {
                                int bx_c, bx_m, bx_n, by_c;
                                window_btns(&windows[f], &bx_c, &bx_m, &bx_n, &by_c);

                                if (mouse_y >= by_c && mouse_y <= by_c + 19) {
                                    if (mouse_x >= bx_c && mouse_x <= bx_c + 23) {
                                        pressed_window_id = f; pressed_window_btn = 1;
                                    } else if (mouse_x >= bx_m && mouse_x <= bx_m + 23) {
                                        pressed_window_id = f; pressed_window_btn = 2;
                                    } else if (mouse_x >= bx_n && mouse_x <= bx_n + 23) {
                                        pressed_window_id = f; pressed_window_btn = 3;
                                    }
                                }
                            }
                            continue;   // il click resta al pannello: nessun cambio di fuoco
                        }

                        // Controlliamo dall'alto verso il basso
                        for (int order_idx = 0; order_idx < MAX_WINDOWS; order_idx++) {
                            int i = window_order[order_idx];
                            
                            // Ignoriamo completamente le finestre ridotte a icona (Ghost Windows)
                            if (windows[i].active && !windows[i].is_minimized) {
                                // Il cursore si trova dentro il perimetro di questa finestra?
                                if (mouse_x >= windows[i].x && mouse_x <= windows[i].x + windows[i].w && 
                                    mouse_y >= windows[i].y && mouse_y <= windows[i].y + windows[i].h) {
                                    bring_to_front(i); // Portala subito in primo piano
                                    global_active_win = i; // RESTITUIAMO IL FOCUS

                                    // =======================================================
                                    // Il Menu Start non ha X, Barra Titolo e Ridimensionamento
                                    // =======================================================
                                    if (windows[i].type != WIN_TYPE_START_MENU && windows[i].type != WIN_TYPE_APP_MENU) {

                                        // =======================================================
                                        // CLICK SULL'ICONA (NUVOLA INFORMATIVA)
                                        // =======================================================
                                        // Da massimizzata l'icona non esiste: quella zona e' il
                                        // pannello dei menu, e il tooltip non va armato.
                                        int icon_x = windows[i].x + 6;
                                        int icon_y = windows[i].y + 8;
                                        // Solo il terminale ha ancora l'icona: altrove
                                        // quella zona ospita il vassoio e non risponde.
                                        if (!windows[i].is_maximized &&
                                            windows[i].type == WIN_TYPE_TERMINAL &&
                                            mouse_x >= icon_x && mouse_x <= icon_x + 16 &&
                                            mouse_y >= icon_y && mouse_y <= icon_y + 16) {
                                            // Effetto Toggle (Accendi/Spegni al click ripetuto)
                                            if (old_tooltip != i) active_tooltip_window = i; 
                                            refresh_requested = 1;
                                            break; // Assorbiamo il click!
                                        }

                                        // Coordinate dei tre comandi: sulla barra del titolo a
                                        // finestra normale, nel pannello dei menu da massimizzata.
                                        int btn_x, max_x, min_x, btn_y;
                                        window_btns(&windows[i], &btn_x, &max_x, &min_x, &btn_y);

                                        // Pulsante X (Chiudi finestra) - ORA "ARMA" SOLO IL CLICK
                                        if (mouse_x >= btn_x && mouse_x <= btn_x + 23 && mouse_y >= btn_y && mouse_y <= btn_y + 19) {
                                            pressed_window_id = i;
                                            pressed_window_btn = 1; // 1 = Azione Chiusura
                                            break; 
                                        }

                                        // Pulsante MAXIMIZE (+) - ORA "ARMA" SOLO IL CLICK
                                        if (mouse_x >= max_x && mouse_x <= max_x + 23 && mouse_y >= btn_y && mouse_y <= btn_y + 19) {
                                            pressed_window_id = i;
                                            pressed_window_btn = 2; // 2 = Azione Massimizza
                                            break;
                                        }

                                        // PULSANTE MINIMIZZA - ORA "ARMA" SOLO IL CLICK
                                        if (mouse_x >= min_x && mouse_x <= min_x + 23 && mouse_y >= btn_y && mouse_y <= btn_y + 19) {
                                            pressed_window_id = i;
                                            pressed_window_btn = 3; // 3 = Azione Minimizza
                                            break;
                                        }


                                        // Barra del Titolo (Trascinamento - BLOCCATO SE MASSIMIZZATA)
                                        if (mouse_x >= windows[i].x + 4 && mouse_x <= windows[i].x + windows[i].w - 4 && 
                                            mouse_y >= windows[i].y + 4 && mouse_y <= windows[i].y + 28 && !windows[i].is_maximized) {
                                            dragged_window = i; 
                                            drag_offset_x = mouse_x - windows[i].x;
                                            drag_offset_y = mouse_y - windows[i].y;
                                            break; 
                                        }

                                        // ANGOLO DI RIDIMENSIONAMENTO (BLOCCATO SE MASSIMIZZATA)
                                        int res_x = windows[i].x + windows[i].w - 16;
                                        int res_y = windows[i].y + windows[i].h - 16;
                                        if (mouse_x >= res_x && mouse_x <= res_x + 16 && mouse_y >= res_y && mouse_y <= res_y + 16 && !windows[i].is_maximized) {
                                            resizing_window = i;
                                            resize_frame_w = windows[i].w;
                                            resize_frame_h = windows[i].h;
                                            break;
                                        }



                                    }


                                    // Logiche Terminale (AGGIORNATE PER DRAG SCROLLBAR)
                                    if (windows[i].type == WIN_TYPE_TERMINAL) {
                                        // Ricalcoliamo le coordinate con i nuovi margini
                                        int term_x = windows[i].x + 4;
                                        int term_y = windows[i].y + 32;
                                        int term_w = windows[i].w - 8;
                                        int term_h = windows[i].h - 36;
                                        
                                        int sb_x = term_x + term_w - 20; 
                                        int sb_y = term_y;  
                                        int sb_h = term_h;                
                                        
                                        // A. CLICK SUL CURSORE DELLA SCROLLBAR (Inizia trascinamento)
                                        if (windows[i].t_max_scrl > 0 && mouse_x >= sb_x && mouse_x <= sb_x + 20 && 
                                            mouse_y >= windows[i].t_thumb_y && mouse_y <= windows[i].t_thumb_y + windows[i].t_thumb_h) {
                                            scrolling_window = i;
                                            scroll_drag_offset_y = mouse_y; // <-- Memorizziamo la Y esatta del mouse
                                            scroll_start_val = windows[i].t_scroll; // <-- Memorizziamo lo scroll esatto
                                            break;
                                        }
                                        // B. Scrollbar SU (Freccia in alto)
                                        else if (mouse_x >= sb_x && mouse_x <= sb_x + 20 && mouse_y >= sb_y && mouse_y <= sb_y + 20) {
                                            windows[i].t_scroll += 2; 
                                        }
                                        // C. Scrollbar GIU (Freccia in basso)
                                        else if (mouse_x >= sb_x && mouse_x <= sb_x + 20 && mouse_y >= sb_y + sb_h - 20 && mouse_y <= sb_y + sb_h) {
                                            windows[i].t_scroll -= 2;
                                            if (windows[i].t_scroll < 0) windows[i].t_scroll = 0;
                                        }
                                        // D. Click sul binario vuoto (Effetto Page Up / Page Down)
                                        else if (windows[i].t_max_scrl > 0 && mouse_x >= sb_x && mouse_x <= sb_x + 20 && 
                                                 mouse_y > sb_y + 20 && mouse_y < sb_y + sb_h - 20) {
                                            if (mouse_y < windows[i].t_thumb_y) windows[i].t_scroll += 10;
                                            else windows[i].t_scroll -= 10;
                                            if (windows[i].t_scroll < 0) windows[i].t_scroll = 0;
                                            if (windows[i].t_scroll > windows[i].t_max_scrl) windows[i].t_scroll = windows[i].t_max_scrl;
                                        }
                                        // E. Click sulla riga di input testuale del terminale
                                        else if (mouse_y >= (term_y + term_h - 40) && mouse_y <= (term_y + term_h - 10)) {
                                            int max_input_chars = (term_w - 60) / 8;
                                            int start_idx = 0;
                                            if (windows[i].t_cursor >= max_input_chars) start_idx = windows[i].t_cursor - max_input_chars + 1;
                                            
                                            int click_x_rel = mouse_x - (term_x + 42); // 10px margine + 32px di "OS> "
                                            if (click_x_rel >= 0) {
                                                int clicked_pos = start_idx + ((click_x_rel + 4) / 8);
                                                if (clicked_pos > windows[i].t_len) clicked_pos = windows[i].t_len;
                                                windows[i].t_cursor = clicked_pos;
                                                blink_counter = 0;
                                                refresh_requested = 1;
                                            }
                                        }
                                    }
                                    
                                    
                                    
                                    // Menu principale: tutto in start_menu_click(). Il vecchio
                                    // sottomenu Applications non esiste piu': le app sono
                                    // direttamente nel menu, e il loro avvio e' launch_app().
                                    if (windows[i].type == WIN_TYPE_START_MENU) {
                                        start_menu_click(i, mouse_x, mouse_y);
                                    }
                                    // =======================================================
                                    // 7. Logiche App GUI (Focus sulle Textbox)
                                    // =======================================================
                                    else if (windows[i].type == WIN_TYPE_APP_GUI) {
                                        int app_x = windows[i].x + 4;
                                        int app_y = windows[i].y + 30; 
                                        int clicked_on_textbox = 0;
                                        
                                        // Scansioniamo tutti gli elementi grafici dell'app per vedere se abbiamo colpito una Textbox
                                        for(int e = 0; e < windows[i].ui_count; e++) {
                                            gui_element_t* el = &windows[i].ui_elements[e];
                                            // AREA DI TESTO MULTILINEA: fuoco, cursore o pagina
                                            if (el->type == 6) {
                                                int ex = app_x + el->x;
                                                int ey = app_y + el->y;
                                                int ti = el->color2 & 0xFF;
                                                if (ti < TA_MAX && windows[i].ta[ti].buf &&
                                                    mouse_x >= ex && mouse_x <= ex + el->w &&
                                                    mouse_y >= ey && mouse_y <= ey + el->h) {
                                                    focused_window_id = i;
                                                    focused_element_idx = e;
                                                    clicked_on_textbox = 1;
                                                    ta_click(&windows[i].ta[ti], el, mouse_x - ex, mouse_y - ey);
                                                    blink_counter = 0;
                                                    refresh_requested = 1;
                                                    break;
                                                }
                                                continue;
                                            }
                                            if (el->type == 4) { 
                                                int ex = app_x + el->x;
                                                int ey = app_y + el->y;
                                                

                                                // Abbiamo cliccato questa Textbox
                                                if (mouse_x >= ex && mouse_x <= ex + el->w && mouse_y >= ey && mouse_y <= ey + el->h) {
                                                    focused_window_id = i;
                                                    focused_element_idx = e;
                                                    clicked_on_textbox = 1;
                                                            
                                                    // =======================================================
                                                    // CALCOLO CLICK PROPORZIONALE (Con Scroll-Awareness)
                                                    // =======================================================
                                                    int t_len = 0; while(el->text[t_len] != '\0') t_len++;
                                                    
                                                    // Ricalcoliamo lo scroll per capire DOVE ESATTAMENTE abbiamo cliccato
                                                    int temp_cursor_offset = 0;
                                                    for (int k = 0; k < focused_cursor_pos && el->text[k] != '\0'; k++) {
                                                        unsigned char uc = (unsigned char)el->text[k];
                                                        if (FONT_HAS(uc)) {
                                                            if (uc == ' ') temp_cursor_offset += 5;
                                                            else temp_cursor_offset += font_prop[uc].width + 1;
                                                        } else temp_cursor_offset += 8;
                                                    }
                                                    
                                                    int text_area_w = el->w - 8;
                                                    int scroll_x = 0;
                                                    if (temp_cursor_offset > text_area_w - 12) {
                                                        scroll_x = temp_cursor_offset - (text_area_w - 12);
                                                    }

                                                    // AGGIUNGIAMO LO SCROLL ALLA X RELATIVA DEL MOUSE
                                                    int click_x_rel = mouse_x - (ex + 4) + scroll_x;
                                                    int current_x = 0;
                                                    int click_pos = 0;
                                                            
                                                    for (int k = 0; k < t_len; k++) {
                                                        unsigned char uc = (unsigned char)el->text[k];
                                                        int char_w = 8;
                                                        if (FONT_HAS(uc)) {
                                                            if (uc == ' ') char_w = 5;
                                                            else char_w = font_prop[uc].width + 1;
                                                        }
                                                                
                                                        if (click_x_rel < current_x + (char_w / 2)) break; 
                                                                
                                                        current_x += char_w;
                                                        click_pos++;
                                                    }
                                                            
                                                    focused_cursor_pos = click_pos;
                                                    blink_counter = 0; 
                                                            
                                                    refresh_requested = 1;
                                                    break;
                                                }

                                            }
                                        }
                                        
                                        // Se clicchiamo sulla finestra, ma NON su una textbox, perdiamo il Focus
                                        if (!clicked_on_textbox && focused_window_id == i) {
                                            focused_window_id = -1;
                                            focused_element_idx = -1;
                                            refresh_requested = 1;
                                        }
                                    }
                                    break; // FONDAMENTALE: Abbiamo cliccato la finestra in cima, ignoriamo le altre
                                }
                            }
                        }
                    }

                    // Se rilasciamo il click, resettiamo tutto
                    // =======================================================
                    // FASE DI RILASCIO: ESEGUIAMO L'AZIONE SE SIAMO ANCORA SUL BOTTONE
                    // =======================================================
                    if (is_clicking == 0 && prev_clicking == 1) {
                        // RILASCIO DELLA MASCOTTE: cerca il vassoio piu' vicino.
                        // Il criterio e' la distanza fra i centri, non la
                        // sovrapposizione: cosi' basta avvicinarsi e lei salta
                        // al posto, invece di dover centrare il bersaglio al pixel.
                        if (mascot_drag) {
                            mascot_drag = 0;
                            refresh_requested = 1;

                            int mcx = mouse_x - mascot_grab_dx + MASCOT_W / 2;
                            int mcy = mouse_y - mascot_grab_dy + MASCOT_H / 2;

                            int best = -2, bestd = 40 * 40;   // 40 px di tolleranza

                            for (int i = 0; i < MAX_WINDOWS; i++) {
                                if (!window_has_tray(i)) continue;
                                int tx = windows[i].x + 14;
                                int ty = windows[i].y + 27;
                                int dx = mcx - tx, dy = mcy - ty;
                                int d = dx * dx + dy * dy;
                                if (d < bestd) { bestd = d; best = i; }
                            }

                            // La taskbar e' un bersaglio come gli altri
                            {
                                int hx = mascot_home_x + MASCOT_W / 2;
                                int hy = mascot_home_y + MASCOT_H / 2;
                                int dx = mcx - hx, dy = mcy - hy;
                                int d = dx * dx + dy * dy;
                                if (d < bestd) { bestd = d; best = -1; }
                            }
                            // Il vassoio della barra superiore, se chi la possiede
                            // e' massimizzato: lasciandola li', quella finestra ne
                            // diventa l'ospite.
                            if (panel_tray_active(global_active_win)) {
                                int dx = mcx - panel_tray_cx();
                                int dy = mcy - (PANEL_TRAY_BASE - 4);
                                int d = dx * dx + dy * dy;
                                if (d < bestd) { bestd = d; best = global_active_win; }
                            }
                            // Nessun bersaglio abbastanza vicino: torna a casa
                            mascot_host = (best == -2) ? -1 : best;

                            prev_clicking = is_clicking;
                            continue;
                        }
                        // ========================================================================
                        // RILASCIO SUL PULSANTE "INFO" (Apertura App)
                        // ========================================================================
                        int tb_y_rel = taskbar_content_y(); 
                        int tb_w_rel = taskbar_w;
                        int tb_x_rel = (fb_width - tb_w_rel) / 2;
                        int clock_x_hit_rel = (tb_x_rel + tb_w_rel) - 33;
                        int info_w_hit_rel = 26;
                        int info_x_hitbox_rel = clock_x_hit_rel - info_w_hit_rel - 10; 
                        
                        // Apriamo l'app SOLO se avevamo premuto lì E stiamo rilasciando lì
                        if (pressed_info_btn && mouse_x >= info_x_hitbox_rel && mouse_x <= info_x_hitbox_rel + info_w_hit_rel && mouse_y >= tb_y_rel && mouse_y <= tb_y_rel + 26) {
                            char target_file[] = "/system/sys_apps/info.edxi";
                            uint32_t actual_size = 0;
                            if (ext2_read_file(target_file, NULL, 0, &actual_size) && actual_size > 0) {
                                uint32_t* app_pt = (uint32_t*)kmalloc(4096);
                                if (app_pt) {
                                    for(int k=0; k<1024; k++) app_pt[k] = 0;
                                    void* temp_buffer = kmalloc(500000); 
                                    if (temp_buffer && ext2_read_file(target_file, (char*)temp_buffer, 500000, &actual_size)) {
                                        uint32_t frames_needed = (actual_size / 4096) + 1;
                                        for(uint32_t f = 0; f < frames_needed; f++) {
                                            uint32_t phys_frame = pfa_alloc_frame();
                                            app_pt[f] = phys_frame | 0x07; 
                                            uint8_t* dest = (uint8_t*)phys_frame; 
                                            uint8_t* src = (uint8_t*)temp_buffer + (f * 4096);
                                            for(int b = 0; b < 4096; b++) {
                                                if ((f * 4096 + b) < actual_size) dest[b] = src[b];
                                                else dest[b] = 0;
                                            }
                                        }
                                        kfree(temp_buffer);
                                        uint8_t* mem_ptr = (uint8_t*)(app_pt[0] & 0xFFFFF000); 
                                        if (mem_ptr[0] == 'E' && mem_ptr[1] == 'D' && mem_ptr[2] == 'X' && mem_ptr[3] == 'I' && mem_ptr[5] == 1) {
                                            int is_gui = (mem_ptr[4] == 1);
                                            void (*app_func)(void) = (void (*)(void))(0x40000000 + 16);
                                            int new_pid = get_free_pid();
                                            if (new_pid != -1) {
                                                int app_win_id = open_window(is_gui ? WIN_TYPE_APP_GUI : WIN_TYPE_TERMINAL, target_file, 250, 200, 250, 180, new_pid);
                                                create_process(app_func, app_win_id, app_pt, 1, -1, new_pid);
                                                refresh_requested = 1;
                                            } else {
                                                for(uint32_t f = 0; f < frames_needed; f++) pfa_free_frame(app_pt[f] & 0xFFFFF000);
                                                kfree(app_pt);
                                            }
                                        } else {
                                            for(uint32_t f = 0; f < frames_needed; f++) pfa_free_frame(app_pt[f] & 0xFFFFF000);
                                            kfree(app_pt);
                                        }
                                    } else {
                                        if (temp_buffer) kfree(temp_buffer);
                                        kfree(app_pt);
                                    }
                                }
                            }
                        }
                        if (pressed_window_id != -1 && windows[pressed_window_id].active) {
                            

                            int i = pressed_window_id;
                            int btn_x, max_x, min_x, btn_y;
                            window_btns(&windows[i], &btn_x, &max_x, &min_x, &btn_y);
                            int max_y = btn_y, min_y = btn_y;

                            // A. Rilascio sul bottone CHIUDI
                            if (pressed_window_btn == 1 && mouse_x >= btn_x && mouse_x <= btn_x + 23 && mouse_y >= btn_y && mouse_y <= btn_y + 19) {
                                
                                // === VERIFICA DELLO SCUDO I/O (Uninterruptible Sleep) ===
                                int can_close = 1;
                                for(int p = 1; p < MAX_PROCESSES; p++) {
                                    // Se un processo collegato a questa finestra sta usando il disco...
                                    if (process_table[p].active && (process_table[p].linked_window == i || process_table[p].parent_window == i)) {
                                        if (process_table[p].is_critical_io == 1) {
                                            can_close = 0; // DIVIETO DI CHIUSURA
                                            break;
                                        }
                                    }
                                }

                                if (can_close == 0) {
                                    // Feedback segreto nel terminale di sistema
                                    print_term("SISTEMA: Chiusura bloccata. L'app sta leggendo/scrivendo sul disco!");
                                } 
                                else {
                                    // NESSUN PERICOLO: Procediamo con l'animazione e la chiusura
                                    /*
                                    anim_start_x = windows[i].x; anim_start_y = windows[i].y; 
                                    anim_start_w = windows[i].w; anim_start_h = windows[i].h;
                                    
                                    // Obiettivo: Collassare al centro lasciando un sottilissimo filo pinstripe di chiusura
                                    anim_target_x = windows[i].x + (windows[i].w / 2) - 2;
                                    anim_target_y = windows[i].y + (windows[i].h / 2) - 2;
                                    anim_target_w = 4; anim_target_h = 4;
                                    
                                    anim_frames_total = 40; anim_frames_left = 40; anim_active = 1;
                                    */
                                    // === NUOVA LOGICA PARENT-CHILD (CHIUSURA) ===
                                    for(int p = 1; p < MAX_PROCESSES; p++) {
                                        // Se è la finestra madre (linked) O il Terminale genitore (parent)
                                        if (process_table[p].active && (process_table[p].linked_window == i || process_table[p].parent_window == i)) {
                                            
                                            // Finestre figlie, memoria e taskbar
                                            close_app(p);
                                        }
                                    }
                                    
                                    // Infine, chiudiamo SEMPRE la finestra
                                    windows[i].active = 0;
                                    update_taskbar_state(); // <--- AGGIUNTO QUI
                                    refresh_requested = 1;
                                }
                            }
                            // B. Rilascio sul bottone MASSIMIZZA
                            else if (pressed_window_btn == 2 && mouse_x >= max_x && mouse_x <= max_x + 23 && mouse_y >= max_y && mouse_y <= max_y + 19) {
                                if (windows[i].is_maximized) {
                                    windows[i].x = windows[i].prev_x; windows[i].y = windows[i].prev_y;
                                    windows[i].w = windows[i].prev_w; windows[i].h = windows[i].prev_h; windows[i].is_maximized = 0;
                                } else {
                                    windows[i].prev_x = windows[i].x; windows[i].prev_y = windows[i].y;
                                    windows[i].prev_w = windows[i].w; windows[i].prev_h = windows[i].h;
                                    // La barra del titolo (32px) scivola sotto
                                    // il pannello, che la copre per intero: il
                                    // contenuto dell'app comincia da solo sotto
                                    // la barra dei menu, senza altri ricalcoli.
                                    windows[i].x = 0;
                                    windows[i].y = PANEL_BAR_H - 32;
                                    windows[i].w = fb_width;
                                    windows[i].h = taskbar_panel_docked_top() - windows[i].y;
                                    windows[i].is_maximized = 1;
                                }
                                update_taskbar_state();
                                refresh_requested = 1;
                            }
                            // C. Rilascio sul bottone MINIMIZZA (Target Predittivo Dinamico)
                            else if (pressed_window_btn == 3 && mouse_x >= min_x && mouse_x <= min_x + 23 && mouse_y >= min_y && mouse_y <= min_y + 19) {
                                int valid_wins[MAX_WINDOWS]; 
                                int valid_wins_count = get_taskbar_windows(valid_wins);
                                int my_tb_index = -1;
                                for(int tw = 0; tw < valid_wins_count; tw++) { if (valid_wins[tw] == i) my_tb_index = tw; }
                                
                                // Calcoliamo la geometria futura della taskbar
                                int max_tb_w = fb_width - 20;
                                int min_tb_w = 220;
                                // Minimizzando, il numero di bottoni sulla barra resta inalterato
                                int fut_tb_w = min_tb_w + (valid_wins_count * 165);
                                if (fut_tb_w > max_tb_w) fut_tb_w = max_tb_w;
                                
                                int fut_tb_x = (fb_width - fut_tb_w) / 2;
                                int fut_start_btn_x = fut_tb_x + 7;
                                int fut_tb_start_x = fut_start_btn_x + 88;
                                
                                int fut_clock_x = (fut_tb_x + fut_tb_w) - 33;
                                int fut_tb_end_x = (fut_clock_x - 26 - 10) - 10;
                                int fut_tb_area_w = fut_tb_end_x - fut_tb_start_x;
                                
                                int target_x = fut_tb_start_x; 
                                int target_y = fb_height - 28; 
                                int target_w = 160;
                                
                                if (valid_wins_count > 0 && fut_tb_area_w > 0) {
                                    int btn_w = fut_tb_area_w / valid_wins_count;
                                    if (btn_w > 160) btn_w = 160;

                                    // La finestra atterra esattamente sul fondo bianco
                                    // che porta il nome dell'app: stessi estremi che usa
                                    // draw_text_shelf, dieci pixel dentro per lato
                                    // (uno solo, per i pulsanti molto stretti).
                                    int actual_w = btn_w - 4;
                                    int bx  = fut_tb_start_x + (my_tb_index * btn_w);
                                    int sx0 = bx + 10;
                                    int sx1 = bx + actual_w - 11;
                                    if (sx1 - sx0 < 6) { sx0 = bx + 1; sx1 = bx + actual_w - 2; }
                                    target_x = sx0;
                                    target_w = sx1 - sx0 + 1;
                                }

                                anim_start_x = windows[i].x; anim_start_y = windows[i].y; 
                                anim_start_w = windows[i].w; anim_start_h = windows[i].h;
                                
                                // --- FIX BUG SFARFALLIO (RACE CONDITION) ---
                                // Sincronizziamo subito i valori attuali con la partenza
                                // in modo che il primo render non usi le vecchie coordinate rimaste sulla taskbar!
                                anim_x = anim_start_x; anim_y = anim_start_y;
                                anim_w = anim_start_w; anim_h = anim_start_h;
                                // -------------------------------------------

                                anim_target_x = target_x; anim_target_y = target_y;
                                anim_target_w = target_w; anim_target_h = 26;
                                
                                anim_frames_total = 40; anim_frames_left = 40; anim_active = 1;

                                windows[i].is_minimized = 1;
                                int pos = -1; for(int m = 0; m < MAX_WINDOWS; m++) { if (window_order[m] == i) pos = m; }
                                if (pos != -1) {
                                    for(int m = pos; m < MAX_WINDOWS - 1; m++) window_order[m] = window_order[m+1];
                                    window_order[MAX_WINDOWS - 1] = i; 
                                }
                                update_taskbar_state();
                                refresh_requested = 1; 
                            }
                        }
                    }

                    // Reset generale incondizionato al momento in cui si alza il dito dal mouse
                                        
                    if (is_clicking == 0) {
                        // Menu principale: la voce premuta si apre adesso, al
                        // rilascio, e solo se il puntatore e' ancora sopra di lei
                        start_menu_release(mouse_x, mouse_y);

                        // Ridimensionamento a cornice: la nuova misura si applica
                        // adesso, e la finestra si ridisegna una volta sola
                        if (resizing_window != -1 && windows[resizing_window].active) {
                            windows[resizing_window].w = resize_frame_w;
                            windows[resizing_window].h = resize_frame_h;
                            refresh_requested = 1;
                        }
                        dragged_window = -1;
                        resizing_window = -1;
                        scrolling_window = -1; 
                        pressed_window_id = -1; // Azzeriamo il focus dei bottoni
                        pressed_window_btn = 0; // Cancelliamo l'intenzione
                        pressed_info_btn = 0;   // Resetta lo stato del tasto Info
                    }

                    // Applico il movimento alla finestra che stiamo trascinando (se esiste)
                    if (dragged_window != -1 && windows[dragged_window].active) {
                        windows[dragged_window].x = mouse_x - drag_offset_x;
                        
                        int new_y = mouse_y - drag_offset_y;
                        
                        // Limite Superiore: le finestre normali si fermano sotto il pannello
                        int top_limit = PANEL_BAR_H;
                        if (new_y < top_limit) new_y = top_limit;
                        
                    
                        // Altezza taskbar (30px) + Altezza barra del titolo (~30px) = 60px di margine
                        if (new_y > fb_height - 60) new_y = fb_height - 60;
                        
                        windows[dragged_window].y = new_y;
                    }

                    // Applico il ridimensionamento (se esiste)
                    if (resizing_window != -1 && windows[resizing_window].active) {
                        // Calcoliamo la nuova grandezza posizionando l'angolo sul cursore
                        int new_w = mouse_x - windows[resizing_window].x + 8;
                        int new_h = mouse_y - windows[resizing_window].y + 8;
                        
                        // I LIMITI DI ROCCIA: Leggiamo il bounding box minimo calcolato dalla Syscall 7
                        int limit_w = windows[resizing_window].min_w;
                        int limit_h = windows[resizing_window].min_h;
                        
                        if (new_w < limit_w) new_w = limit_w; 
                        if (new_h < limit_h) new_h = limit_h; 
                        
                        // Si muove solo la cornice: la finestra cambiera' misura
                        // una volta sola, al rilascio del mouse
                        if (new_w != resize_frame_w || new_h != resize_frame_h) {
                            resize_frame_w = new_w;
                            resize_frame_h = new_h;
                            refresh_requested = 1;
                        }
                    }
                    // Applico il trascinamento della scrollbar del terminale
                    if (scrolling_window != -1 && windows[scrolling_window].active) {
                        int i = scrolling_window;
                        int sb_h = windows[i].h - 36;
                        int track_h = sb_h - 40;
                        int draggable_space = track_h - windows[i].t_thumb_h;
                        
                        if (draggable_space > 0) {
                            // 1. Calcoliamo SOLO la differenza di movimento del mouse
                            int delta_y = mouse_y - scroll_drag_offset_y;
                            
                            // 2. Traduciamo i pixel spostati in "righe di testo" da far scorrere
                            int delta_scroll = (delta_y * windows[i].t_max_scrl) / draggable_space;
                            
                            // 3. Applichiamo la differenza al valore iniziale
                            // (Sottraiamo perché trascinare GIÙ significa andare verso la riga 0)
                            windows[i].t_scroll = scroll_start_val - delta_scroll;
                            
                            // 4. Limiti di sicurezza
                            if (windows[i].t_scroll < 0) windows[i].t_scroll = 0;
                            if (windows[i].t_scroll > windows[i].t_max_scrl) windows[i].t_scroll = windows[i].t_max_scrl;
                        }
                    }
                    
                }
            } else {
                // --- LA TASTIERA ---
                
                uint8_t scancode = inb(0x60);
                int top_win = global_active_win; // Usa il vero focus
                
                int app_pid = -1;
                
                if (top_win != -1) { // PROTEZIONE: Leggiamo solo se una finestra ha il focus!
                    for(int p=1; p<MAX_PROCESSES; p++) {
                        // Cerca se c'è un'app ancorata a questa finestra (App testuale) 
                        // OPPURE se c'è un'app figlia di questa finestra (App GUI lanciata dal terminale)
                        if (process_table[p].active && 
                           (process_table[p].linked_window == top_win || process_table[p].parent_window == top_win)) {
                            app_pid = p;
                        }
                    }

                    // SVEGLIA INTELLIGENTE
                    for(int p=1; p<MAX_PROCESSES; p++) {
                        if (process_table[p].active && process_table[p].waiting_for_input == 2) {
                            if (windows[top_win].owner_pid == p) {
                                process_table[p].waiting_for_input = 0; 
                            }
                        }
                    }
                }



                // 1. GESTIONE GLOBALE DEL TASTO SHIFT
                if (scancode == 0x2A || scancode == 0x36) shift_pressed = 1; 
                else if (scancode == 0xAA || scancode == 0xB6) shift_pressed = 0;

                // MENU PRINCIPALE APERTO: la tastiera e' tutta sua, per la ricerca.
                // Il tasto consumato diventa un codice di rilascio senza significato,
                // cosi' nessun altro lo vede: ne' le textbox, ne' l'app sotto il menu.
                {
                    int sm_open = start_menu_find();
                    if (sm_open != -1 && scancode < 128 && scancode != 0x2A && scancode != 0x36) {
                        start_menu_key(sm_open, scancode);
                        scancode = 0x80;
                    }
                }


                // =======================================================
                // AREA DI TESTO MULTILINEA: frecce e pagine
                // Se il fuoco e' su un'area, le frecce sono sue: ta_nav
                // impedisce che le gestisca anche la textbox a riga singola.
                // =======================================================
                int ta_nav = 0;
                {
                    textarea_t* fta;
                    gui_element_t* fel;
                    if (ta_focus(top_win, &fta, &fel)) {
                        if      (scancode == 0x4B) { ta_move_h(fta, fel, -1);               ta_nav = 1; }
                        else if (scancode == 0x4D) { ta_move_h(fta, fel, +1);               ta_nav = 1; }
                        else if (scancode == 0x48) { ta_move_v(fta, fel, -1);               ta_nav = 1; }
                        else if (scancode == 0x50) { ta_move_v(fta, fel, +1);               ta_nav = 1; }
                        else if (scancode == 0x49) { ta_move_v(fta, fel, -ta_visible(fel)); ta_nav = 1; }
                        else if (scancode == 0x51) { ta_move_v(fta, fel, +ta_visible(fel)); ta_nav = 1; }
                        if (ta_nav) { blink_counter = 0; refresh_requested = 1; }
                    }
                }

                // =======================================================
                // NUOVO: FRECCE DIREZIONALI PER LA TEXTBOX
                // Scancode: 0x4B (Freccia Sinistra), 0x4D (Freccia Destra)
                // =======================================================
                if (!ta_nav && scancode == 0x4B) { // Freccia Sinistra
                    if (focused_window_id != -1 && focused_window_id == top_win) { // Textbox GUI
                        if (focused_cursor_pos > 0) focused_cursor_pos--;
                        blink_counter = 0; refresh_requested = 1;
                    } 
                    else if (windows[top_win].active && windows[top_win].type == WIN_TYPE_TERMINAL && app_pid == -1) { // Terminale
                        if (term_cursor > 0) term_cursor--;
                        blink_counter = 0; refresh_requested = 1;
                    }
                }
                else if (!ta_nav && scancode == 0x4D) { // Freccia Destra
                    if (focused_window_id != -1 && focused_window_id == top_win) { // Textbox GUI
                        gui_element_t* el = &windows[focused_window_id].ui_elements[focused_element_idx];
                        int t_len = 0; while (el->text[t_len] != '\0') t_len++;
                        if (focused_cursor_pos < t_len) focused_cursor_pos++;
                        blink_counter = 0; refresh_requested = 1;
                    } 
                    else if (windows[top_win].active && windows[top_win].type == WIN_TYPE_TERMINAL && app_pid == -1) { // Terminale
                        if (term_cursor < term_len) term_cursor++;
                        blink_counter = 0; refresh_requested = 1;
                    }
                }


                if (scancode < 128) {
                    char ascii = scancode_to_ascii[scancode];
                    
                    if (ascii != 0) {
                        // 2. TRADUZIONE SIMBOLI UNIVERSALE (Valida per Kernel e App)
                        if (shift_pressed) {
                            if (ascii >= 'a' && ascii <= 'z') ascii -= 32; 
                            else if (ascii == '1') ascii = '!';
                            else if (ascii == '9') ascii = '(';
                            else if (ascii == '0') ascii = ')';
                            else if (ascii == '[') ascii = '{';
                            else if (ascii == ']') ascii = '}';
                            else if (ascii == ';') ascii = ':';
                            else if (ascii == ',') ascii = '<';
                            else if (ascii == '.') ascii = '>';
                            else if (ascii == '/') ascii = '?';
                            else if (ascii == '-') ascii = '_';
                            else if (ascii == '=') ascii = '+';
                            else if (ascii == '\'') ascii = '"';
                        }
                        
                        // LA PATCH DEFINITIVA ANTI-FANTASMA
                        if (app_pid > 0) last_key_pressed = ascii;
                        else last_key_pressed = 0;


                        // Un'app grafica che dorme in gui_yield va svegliata anche
                        // per un tasto, non solo per il mouse: altrimenti leggerebbe
                        // il tasto soltanto al primo movimento del mouse.
                        if (app_pid > 0 && process_table[app_pid].waiting_for_input == 2)
                            process_table[app_pid].waiting_for_input = 0;

                        
                        // =======================================================
                        // CASO 0-BIS: STIAMO SCRIVENDO IN UN'AREA MULTILINEA
                        // =======================================================
                        {
                            textarea_t* fta;
                            gui_element_t* fel;
                            if (ta_focus(top_win, &fta, &fel)) {
                                ta_key(fta, fel, ascii);
                                blink_counter = 0;
                                refresh_requested = 1;
                            }
                        }

                        if (focused_window_id != -1 && focused_window_id == top_win && focused_element_idx != -1 &&
                            windows[focused_window_id].ui_elements[focused_element_idx].type != 6) {
                            gui_element_t* el = &windows[focused_window_id].ui_elements[focused_element_idx];
                            
                            int t_len = 0; 
                            while (el->text[t_len] != '\0') t_len++;
                            
                            // BACKSPACE: Cancelliamo la lettera PRIMA del cursore
                            if (ascii == '\b') {
                                if (focused_cursor_pos > 0) {
                                    // Spostiamo tutto l'array a sinistra (incluso il terminatore \0)
                                    for (int k = focused_cursor_pos; k <= t_len; k++) {
                                        el->text[k - 1] = el->text[k];
                                    }
                                    focused_cursor_pos--;
                                }
                            } 
                            // CARATTERI NORMALI: Inseriamo esattamente in mezzo
                            else if (ascii >= 32 && ascii <= 126) {
                                int max_chars = el->color2; 
                                if (max_chars <= 0 || max_chars > 250) max_chars = 250;
                                
                                if (t_len < max_chars) {
                                    // Spostiamo tutto l'array a destra per fare spazio (incluso il \0)
                                    for (int k = t_len; k >= focused_cursor_pos; k--) {
                                        el->text[k + 1] = el->text[k];
                                    }
                                    el->text[focused_cursor_pos] = ascii;
                                    focused_cursor_pos++;
                                }
                            }
                            
                            blink_counter = 0; // Teniamo il cursore acceso mentre digitiamo
                            refresh_requested = 1; 
                        }

                        // CASO 1: STIAMO SCRIVENDO DENTRO UN'APP
                        if (app_pid > 0 && process_table[app_pid].waiting_for_input) {
                            
                            // ==========================================
                            // Montiamo la Page Table dell'App
                            // prima di scrivere nel suo buffer (Ring 3)!
                            // ==========================================
                            uint32_t flags;
                            asm volatile("pushf \n pop %0 \n cli" : "=r"(flags));
                            
                            // Colleghiamo l'indirizzo 0x40000000 alla RAM vera di questa App
                            page_directory[256] = ((uint32_t)process_table[app_pid].page_table) | 0x07;
                            asm volatile("mov %%cr3, %%eax; mov %%eax, %%cr3" ::: "eax"); // Flush TLB


                            if (ascii == '\n') {
                                process_table[app_pid].input_buffer[process_table[app_pid].input_idx] = '\0';
                                
                                int old_pid = current_pid; current_pid = app_pid; 
                                print_term(""); 
                                current_pid = old_pid;
                                
                                process_table[app_pid].waiting_for_input = 0; 
                            }
                            else if (ascii == '\b') {
                                if (process_table[app_pid].input_idx > 0) {
                                    process_table[app_pid].input_idx--;
                                    process_table[app_pid].input_buffer[process_table[app_pid].input_idx] = '\0';
                                    
                                    int last_row = windows[top_win].t_head - 1;
                                    if (last_row < 0) last_row = TERM_MAX_ROWS - 1;
                                    windows[top_win].t_history[last_row][process_table[app_pid].input_idx + 2] = '\0'; 
                                }
                            }
                            else if (ascii >= 32 && ascii <= 126 && process_table[app_pid].input_idx < process_table[app_pid].input_max - 1) {
                                process_table[app_pid].input_buffer[process_table[app_pid].input_idx] = ascii;
                                process_table[app_pid].input_idx++;
                                process_table[app_pid].input_buffer[process_table[app_pid].input_idx] = '\0';
                                
                                int last_row = windows[top_win].t_head - 1;
                                if (last_row < 0) last_row = TERM_MAX_ROWS - 1;
                                windows[top_win].t_history[last_row][process_table[app_pid].input_idx + 1] = ascii;
                                windows[top_win].t_history[last_row][process_table[app_pid].input_idx + 2] = '\0';
                            }

                            // ==========================================
                            // FINE ZONA CRITICA: Smontiamo la Page Table dell'App
                            // e riaccendiamo gli interrupt
                            // ==========================================
                            page_directory[256] = 0;
                            asm volatile("mov %%cr3, %%eax; mov %%eax, %%cr3" ::: "eax");
                            asm volatile("push %0 \n popf" :: "r"(flags));
                        }
                        // CASO 2: STIAMO SCRIVENDO NEL TERMINALE DEL KERNEL
                        else if (top_win != -1 && windows[top_win].active && windows[top_win].type == WIN_TYPE_TERMINAL && app_pid == -1) {
                            if (ascii == '\b') {
                                if (term_cursor > 0) {
                                    // Spostiamo tutto l'array a sinistra (sovrascrivendo la lettera)
                                    for (int k = term_cursor; k <= term_len; k++) {
                                        term_buffer[k - 1] = term_buffer[k];
                                    }
                                    term_cursor--;
                                    term_len--;
                                }
                            }
                            else if (ascii == '\n') {
                                term_buffer[term_len] = '\0';
                                
                                char echo_buf[260]; // Espanso per contenere i nuovi comandi massicci
                                echo_buf[0] = '>'; echo_buf[1] = ' '; 
                                int e_idx = 2;
                                for(int i=0; i<term_len && e_idx < 258; i++) echo_buf[e_idx++] = term_buffer[i];
                                echo_buf[e_idx] = '\0';
                                
                                if (term_len > 0) print_term(echo_buf); 
                                
                                // =======================================================
                                // MACCHINA A STATI: INTERCETTAZIONE RISPOSTA (y/n)
                                // =======================================================
                                if (pending_rmdir == 1) {
                                    if (term_buffer[0] == 'y' || term_buffer[0] == 'Y') {
                                        print_term("Inizio protocollo di annientamento ricorsivo...");
                                        render_desktop(mouse_x, mouse_y, is_clicking);
                                        
                                        // CHIAMATA ALLA VERA API DEL KERNEL!
                                        int res = ext2_remove_directory(pending_rmdir_path);
                                        
                                        if (res == 1) print_term("SUCCESSO! Cartella e contenuto vaporizzati.");
                                        else if (res == -1) print_term("ERRORE: Il percorso punta a un file. Usa 'rm'.");
                                        else print_term("ERRORE: Impossibile eliminare (percorso errato).");
                                        
                                        pending_rmdir = 0;
                                    }
                                    else if (term_buffer[0] == 'n' || term_buffer[0] == 'N') {
                                        print_term("Operazione annullata. La cartella e' salva.");
                                        pending_rmdir = 0;
                                    } 
                                    else {
                                        print_term("Rispondi 'y' per confermare o 'n' per annullare.");
                                    }
                                    
                                    term_len = 0;
                                    term_cursor = 0; 
                                    continue; // Salta tutti gli altri comandi
                                }


                                if (term_buffer[0] == 'h' && term_buffer[1] == 'e' && term_buffer[2] == 'l' && term_buffer[3] == 'p') {
                                    
                                    print_term("===== AIUTO DI SISTEMA (ANTE-M OS) =====");
                                    print_term(" - help : Mostra questo menu");
                                    print_term("--- FILE SYSTEM ---");
                                    print_term(" - ls [path]        : Elenca file e directory");
                                    print_term(" - mkdir [path]     : Crea una nuova cartella");
                                    print_term(" - rmdir [path]     : Rimuove dir, dub-dir e file");
                                    print_term(" - cp [file] [path] : copia un file in una cartella");
                                    print_term(" - cat [path]       : Legge il contenuto di un file");
                                    print_term(" - rm [path]        : Elimina file (non cartelle)");
                                    print_term(" - fsinfo           : Statistiche disco Ext2");
                                    print_term("--- SISTEMA E MULTITASKING ---");
                                    print_term(" - run [app]        : Lancia eseguibile .edxi");
                                    print_term(" - ps               : Lista processi attivi");
                                    print_term(" - kill [PID]       : Termina un processo");
                                    print_term(" - meminfo          : Stato Heap e RAM Kernel");
                                    print_term(" - time             : Data e ora dal chip CMOS");
                                    print_term(" - pci              : Scansione bus PCI");
                                    print_term(" - info / clear     : Info sistema / Pulisci");
                                    print_term("=========================================");

                                }

                                // COMANDO THEME (Carica un file JSON e applica i colori)
                                else if (term_buffer[0] == 't' && term_buffer[1] == 'h' && term_buffer[2] == 'e' && term_buffer[3] == 'm' && term_buffer[4] == 'e' && term_buffer[5] == ' ') {
                                    char theme_path[64];
                                    int t_idx = 0; int idx = 6;
                                    
                                    // Aggiungiamo lo slash iniziale se manca
                                    if (term_buffer[idx] != '/') theme_path[t_idx++] = '/';
                                    
                                    while(term_buffer[idx] != '\0' && term_buffer[idx] != ' ' && t_idx < 63) {
                                        theme_path[t_idx++] = term_buffer[idx++];
                                    }
                                    theme_path[t_idx] = '\0';
                                    
                                    print_term("Applicazione Tema Grafico in corso...");
                                    render_desktop(mouse_x, mouse_y, is_clicking); // Render forza visiva
                                    
                                    if (load_theme(theme_path)) {
                                        print_term("SUCCESSO! Tema applicato.");
                                    } else {
                                        print_term("ERRORE: Tema non trovato o file JSON non valido.");
                                    }
                                }

                                else if (term_buffer[0] == 'i' && term_buffer[1] == 'n' && term_buffer[2] == 'f' && term_buffer[3] == 'o') {
                                    print_term("Ante-Millennium OS v0.1 - Architettura x86");
                                    print_term("Alpha - Build 110");
                                    print_term("Developed by Alberto Sanfelice");
                                    print_term("511utilities.org");
                                }
                                else if (term_buffer[0] == 'c' && term_buffer[1] == 'l' && term_buffer[2] == 'e' && term_buffer[3] == 'a' && term_buffer[4] == 'r') {
                                    for(int i=0; i<TERM_MAX_ROWS; i++) term_history[i][0] = '\0'; 
                                }
                                else if (term_buffer[0] == 'r' && term_buffer[1] == 'e' && term_buffer[2] == 'a' && term_buffer[3] == 'd' && term_buffer[4] == '\0') {
                                    print_term("Lettura disco in corso...");
                                    render_desktop(mouse_x, mouse_y, is_clicking);
                                    
                                    uint8_t sector_buffer[512]; 
                                    if (ata_read_sector(0, sector_buffer)) {
                                        char disk_msg[55];
                                        int i;
                                        for(i = 0; i < 50; i++) {
                                            if (sector_buffer[i] >= 32 && sector_buffer[i] <= 126) disk_msg[i] = sector_buffer[i];
                                            else disk_msg[i] = '.';
                                        }
                                        disk_msg[i] = '\0';
                                        print_term("Dati LBA 0:");
                                        print_term(disk_msg);
                                    } else {
                                        print_term("ERRORE: Disco non trovato!");
                                    }
                                }
                                else if (term_buffer[0] == 'w' && term_buffer[1] == 'r' && term_buffer[2] == 'i' && term_buffer[3] == 't' && term_buffer[4] == 'e') {
                                    print_term("Scrittura su disco in corso...");
                                    render_desktop(mouse_x, mouse_y, is_clicking);
                                    
                                    uint8_t sector_buffer[512]; 
                                    for(int i=0; i<512; i++) {sector_buffer[i] = 0;}

                                    int i = 6;
                                    int buf_idx = 0;
                                    while(term_buffer[i] != '\0' && buf_idx < 511) {
                                        sector_buffer[buf_idx] = term_buffer[i];
                                        i++;
                                        buf_idx++;
                                    }

                                    if (ata_write_sector(1, sector_buffer)) {
                                        print_term("SUCCESSO: Dati incisi sul disco!");
                                    } else {
                                        print_term("ERRORE: Scrittura fallita.");
                                    }
                                }
                                else if (term_buffer[0] == 'r' && term_buffer[1] == 'e' && term_buffer[2] == 'a' && term_buffer[3] == 'd' && term_buffer[4] == '1') {
                                    print_term("Lettura LBA 1 in corso...");
                                    render_desktop(mouse_x, mouse_y, is_clicking);
                                    
                                    uint8_t sector_buffer[512]; 
                                    if (ata_read_sector(1, sector_buffer)) {
                                        char disk_msg[55];
                                        int i;
                                        for(i = 0; i < 50; i++) {
                                            if (sector_buffer[i] == 0) break; 
                                            if (sector_buffer[i] >= 32 && sector_buffer[i] <= 126) disk_msg[i] = sector_buffer[i];
                                            else disk_msg[i] = '.';
                                        }
                                        disk_msg[i] = '\0';
                                        print_term("Contenuto Settore 1:");
                                        print_term(disk_msg);
                                    } else {
                                        print_term("ERRORE: Lettura fallita.");
                                    }
                                }
                                // COMANDO TIME
                                else if (term_buffer[0] == 't' && term_buffer[1] == 'i' && term_buffer[2] == 'm' && term_buffer[3] == 'e' && term_buffer[4] == '\0') {
                                    read_rtc();
                                    char t_msg[] = "Data e Ora: 00:00:00 - 00/00/2000";
                                    put_2digits(rtc_hour, t_msg, 12);
                                    put_2digits(rtc_minute, t_msg, 15);
                                    put_2digits(rtc_second, t_msg, 18);
                                    put_2digits(rtc_day, t_msg, 23);
                                    put_2digits(rtc_month, t_msg, 26);
                                    if (rtc_year < 80) { t_msg[29] = '2'; t_msg[30] = '0'; } 
                                    else { t_msg[29] = '1'; t_msg[30] = '9'; }
                                    put_2digits(rtc_year, t_msg, 31);
                                    
                                    print_term(t_msg);
                                }
                            
                                // COMANDO HTTP (Prova TCP verso Ollama)
                                else if (term_buffer[0] == 'h' && term_buffer[1] == 't' && term_buffer[2] == 't' &&
                                         term_buffer[3] == 'p' && term_buffer[4] == '\0') {
                                    net_http_test();
                                }

                                // COMANDO PING (Prova la rete verso il Mac)
                                else if (term_buffer[0] == 'p' && term_buffer[1] == 'i' && term_buffer[2] == 'n' &&
                                         term_buffer[3] == 'g' && term_buffer[4] == '\0') {
                                    net_ping();
                                }

                                // COMANDO NET (Stato della scheda di rete)
                                else if (term_buffer[0] == 'n' && term_buffer[1] == 'e' && term_buffer[2] == 't' && term_buffer[3] == '\0') {
                                    net_report();
                                }

                                // COMANDO ARPING (Primo pacchetto verso il mondo esterno)
                                else if (term_buffer[0] == 'a' && term_buffer[1] == 'r' && term_buffer[2] == 'p' &&
                                         term_buffer[3] == 'i' && term_buffer[4] == 'n' && term_buffer[5] == 'g' &&
                                         term_buffer[6] == '\0') {
                                    net_arping();
                                }

                                // COMANDO PCI (Scansione hardware)
                                else if (term_buffer[0] == 'p' && term_buffer[1] == 'c' && term_buffer[2] == 'i' && term_buffer[3] == '\0') {
                                    pci_scan();
                                }


                                // COMANDO FSINFO (Interroga il file system ext2)
                                else if (term_buffer[0] == 'f' && term_buffer[1] == 's' && term_buffer[2] == 'i' && term_buffer[3] == 'n' && term_buffer[4] == 'f' && term_buffer[5] == 'o') {
                                    uint8_t sector_buffer[512] = {0}; 
                                    
                                    // Leggiamo LBA 2 (che contiene il Byte 1024)
                                    if (ata_read_sector(2, sector_buffer)) {
                                        
                                        // IL TRUCCO ANTI-COMPILATORE: Copia manuale della memoria
                                        ext2_superblock_t sb;
                                        uint8_t* dst = (uint8_t*)&sb;
                                        for(size_t i = 0; i < sizeof(ext2_superblock_t); i++) {
                                            dst[i] = sector_buffer[i];
                                        }
                                        
                                        // Ora usiamo 'sb.' invece di 'sb->' perché è una vera variabile!
                                        if (sb.s_magic == 0xEF53) {
                                            print_term("Rilevato File System EXT2 Valido");
                                            
                                            char num_buf[32];
                                            char msg[64];
                                            
                                            // Stampa Inodes
                                            itoa(sb.s_inodes_count, num_buf);
                                            msg[0]='\0'; 
                                            int idx = 0; char* label = "Inodes totali: ";
                                            while(*label) msg[idx++] = *label++;
                                            char* val = num_buf;
                                            while(*val) msg[idx++] = *val++;
                                            msg[idx] = '\0';
                                            print_term(msg);

                                            // Stampa Blocchi
                                            itoa(sb.s_blocks_count, num_buf);
                                            msg[0]='\0'; idx=0; label = "Blocchi totali: ";
                                            while(*label) msg[idx++] = *label++;
                                            val = num_buf;
                                            while(*val) msg[idx++] = *val++;
                                            msg[idx] = '\0';
                                            print_term(msg);
                                            
                                        } else {
                                            print_term("File system sconosciuto o non formattato.");
                                        }
                                    } else {
                                        print_term("ERRORE: Impossibile leggere il disco.");
                                    }
                                }
                                
                                // COMANDO LS (Esplora la directory specificata, o Root di default)
                                else if (term_buffer[0] == 'l' && term_buffer[1] == 's' && (term_buffer[2] == '\0' || term_buffer[2] == ' ')) {
                                    char target_dir[64];
                                    
                                    // Se scrivo solo "ls", il percorso di default è "/"
                                    if (term_buffer[2] == '\0') {
                                        target_dir[0] = '/'; target_dir[1] = '\0';
                                    } else {
                                        int idx = 3;
                                        int f_idx = 0;
                                        // Se non metto lo slash iniziale, lo aggiunge il Kernel
                                        if (term_buffer[idx] != '/') {
                                            target_dir[f_idx++] = '/';
                                        }
                                        while (term_buffer[idx] != ' ' && term_buffer[idx] != '\0' && f_idx < 63) {
                                            target_dir[f_idx++] = term_buffer[idx++];
                                        }
                                        target_dir[f_idx] = '\0';
                                    }

                                    // Chiediamo al Path Parser l'Inode della cartella
                                    uint32_t target_inode_num = ext2_resolve_path(target_dir);
                                    
                                    if (target_inode_num == 0) {
                                        print_term("ERRORE: Cartella inesistente o percorso errato.");
                                    } else {
                                        uint8_t sector_buffer[1024] = {0};
                                        if (ata_read_sector(4, sector_buffer)) {
                                            ext2_bg_desc_t bgd; uint8_t* dst = (uint8_t*)&bgd;
                                            for(size_t i=0; i<sizeof(ext2_bg_desc_t); i++) dst[i] = sector_buffer[i];
                                            uint32_t inode_table_lba = bgd.bg_inode_table * 2;
                                            
                                            uint32_t inode_size = 128;
                                            if (ata_read_sector(inode_table_lba, sector_buffer)) {
                                                ext2_inode_t root_test; dst = (uint8_t*)&root_test;
                                                for(size_t i=0; i<sizeof(ext2_inode_t); i++) dst[i] = sector_buffer[128 + i];
                                                if ((root_test.i_mode & 0xF000) != 0x4000) inode_size = 256;

                                                // Carichiamo l'Inode della NOSTRA cartella bersaglio!
                                                uint32_t index = target_inode_num - 1;
                                                uint32_t sect_off = (index * inode_size) / 512;
                                                uint32_t in_sect_off = (index * inode_size) % 512;

                                                if (ata_read_sector(inode_table_lba + sect_off, sector_buffer)) {
                                                    ext2_inode_t target_inode; dst = (uint8_t*)&target_inode;
                                                    for(size_t i=0; i<sizeof(ext2_inode_t); i++) dst[i] = sector_buffer[in_sect_off + i];

                                                    // Muro di Mattoni: Ci assicuriamo che l'utente non abbia provato a fare "ls file.txt"
                                                    if ((target_inode.i_mode & 0xF000) == 0x4000) {
                                                        uint32_t dir_block_lba = target_inode.i_block[0] * 2;
                                                        
                                                        if (ata_read_sector(dir_block_lba, sector_buffer) && ata_read_sector(dir_block_lba + 1, sector_buffer + 512)) {
                                                            char msg_start[100];
                                                            int s_idx = 0; char* lbl = "Contenuto di ";
                                                            while(*lbl) msg_start[s_idx++] = *lbl++;
                                                            char* val = target_dir;
                                                            while(*val && s_idx < 98) msg_start[s_idx++] = *val++;
                                                            msg_start[s_idx++] = ':'; msg_start[s_idx] = '\0';
                                                            print_term(msg_start);
                                                            
                                                            uint32_t offset = 0;
                                                            while (offset < 1024) {
                                                                ext2_dir_entry_t entry; uint8_t* edst = (uint8_t*)&entry;
                                                                for(size_t i = 0; i < sizeof(ext2_dir_entry_t); i++) edst[i] = sector_buffer[offset + i];
                                                                if (entry.inode == 0) break; 
                                                                
                                                                char filename[256];
                                                                for (int k = 0; k < entry.name_len; k++) filename[k] = sector_buffer[offset + 8 + k];
                                                                filename[entry.name_len] = '\0';
                                                                
                                                                char msg[64]; int idx = 0;
                                                                if (entry.file_type == 2) {
                                                                    msg[idx++]='['; msg[idx++]='D'; msg[idx++]='I'; msg[idx++]='R'; msg[idx++]=' '; msg[idx++]=']'; msg[idx++]=' ';
                                                                } else {
                                                                    int len = entry.name_len; int is_app = 0;
                                                                    if (len >= 5 && filename[len-5] == '.' && filename[len-4] == 'e' && filename[len-3] == 'd' && filename[len-2] == 'x' && filename[len-1] == 'i') is_app = 1;
                                                                    if (len >= 4 && filename[len-4] == '.' && filename[len-3] == 'b' && filename[len-2] == 'i' && filename[len-1] == 'n') is_app = 1;
                                                                    
                                                                    if (is_app) { msg[idx++]='['; msg[idx++]='A'; msg[idx++]='P'; msg[idx++]='P'; msg[idx++]=' '; msg[idx++]=']'; msg[idx++]=' '; } 
                                                                    else { msg[idx++]='['; msg[idx++]='F'; msg[idx++]='I'; msg[idx++]='L'; msg[idx++]='E'; msg[idx++]=']'; msg[idx++]=' '; }
                                                                }
                                                                
                                                                val = filename; while(*val && idx < 63) msg[idx++] = *val++;
                                                                msg[idx] = '\0';
                                                                print_term(msg);
                                                                
                                                                if (entry.rec_len == 0) break;
                                                                offset += entry.rec_len;
                                                            }
                                                        } else print_term("Errore lettura dati Directory.");
                                                    } else print_term("Errore: Il percorso non e' una cartella.");
                                                } else print_term("Errore lettura Inode del percorso.");
                                            } else print_term("Errore lettura Inode Table.");
                                        } else print_term("Errore lettura BGDT.");
                                    }
                                }

                                // COMANDO CAT (Es: "cat /users/admin/docs/test.txt")
                                else if (term_buffer[0] == 'c' && term_buffer[1] == 'a' && term_buffer[2] == 't' && term_buffer[3] == ' ') {
                                    char full_file[64];
                                    int idx = 4;
                                    int f_idx = 0;
                                    
                                    // Aggiunge lo slash se dimenticato
                                    if (term_buffer[idx] != '/') full_file[f_idx++] = '/';
                                    while(term_buffer[idx] != '\0' && term_buffer[idx] != ' ' && f_idx < 63) {
                                        full_file[f_idx++] = term_buffer[idx++];
                                    }
                                    full_file[f_idx] = '\0';

                                    char* file_buf = (char*)kmalloc(4096); // Usiamo l'Heap del Kernel in sicurezza
                                    if (file_buf) {
                                        // Lasciamo fare TUTTO il lavoro sporco alla nuova API!
                                        if (ext2_read_file(full_file, file_buf, 4095, 0)) {
                                            print_term("--- INIZIO FILE ---");
                                            char line_buf[513];
                                            int line_idx = 0;
                                            for(int j=0; file_buf[j] != '\0'; j++) {
                                                char c = file_buf[j];
                                                if (c == '\n') {
                                                    line_buf[line_idx] = '\0';
                                                    print_term(line_buf);
                                                    line_idx = 0;
                                                } else if (c >= 32 && c <= 126 && line_idx < 512) {
                                                    line_buf[line_idx++] = c;
                                                }
                                            }
                                            if (line_idx > 0) {
                                                line_buf[line_idx] = '\0';
                                                print_term(line_buf);
                                            }
                                            print_term("--- FINE FILE ---");
                                        } else {
                                            print_term("ERRORE: File non trovato o percorso errato.");
                                        }
                                        kfree(file_buf);
                                    } else {
                                        print_term("ERRORE: RAM insufficiente per cat.");
                                    }
                                }

                                // COMANDO MEMINFO (Stato della RAM)
                                else if (term_buffer[0] == 'm' && term_buffer[1] == 'e' && term_buffer[2] == 'm' && term_buffer[3] == 'i' && term_buffer[4] == 'n' && term_buffer[5] == 'f' && term_buffer[6] == 'o') {
                                    
                                    // 1. STATISTICHE HEAP DEL KERNEL (kmalloc)
                                    size_t total_free_k = 0;
                                    size_t total_used_k = 0;
                                    mem_block_t* current = heap_head;
                                    while (current != NULL) {
                                        if (current->is_free) total_free_k += current->size;
                                        else total_used_k += current->size;
                                        current = current->next;
                                    }
                                    
                                    // 2. STATISTICHE RAM FISICA DELLE APP (Demand Paging / PFA)
                                    uint32_t pfa_free_frames = 0;
                                    uint32_t pfa_used_frames = 0;
                                    for (uint32_t i = 0; i < (pfa_max_frames / 32) + 1; i++) {
                                        for (int bit = 0; bit < 32; bit++) {
                                            uint32_t frame_index = (i * 32) + bit;
                                            if (frame_index >= pfa_max_frames) break; // Non contare l'esubero
                                            
                                            if ((pfa_bitmap[i] & (1 << bit)) == 0) pfa_free_frames++;
                                            else pfa_used_frames++;
                                        }
                                    }
                                    
                                    // Convertiamo i frame in Megabyte per una lettura più facile
                                    uint32_t pfa_free_mb = (pfa_free_frames * 4096) / (1024 * 1024);
                                    uint32_t pfa_used_mb = (pfa_used_frames * 4096) / (1024 * 1024);
                                    
                                    char num_buf[32];
                                    char msg[64];
                                    
                                    print_term("=== STATO DELLA MEMORIA (III-M OS) ===");
                                    print_term("[ RING 0 - KERNEL HEAP ]");
                                    
                                    itoa(total_used_k, num_buf);
                                    msg[0]='\0'; int idx=0; char* label = "  Utilizzata (Bytes) : ";
                                    while(*label) msg[idx++] = *label++;
                                    char* val = num_buf; while(*val) msg[idx++] = *val++; msg[idx]='\0';
                                    print_term(msg);

                                    itoa(total_free_k, num_buf);
                                    msg[0]='\0'; idx=0; label = "  Libera (Bytes)     : ";
                                    while(*label) msg[idx++] = *label++;
                                    val = num_buf; while(*val) msg[idx++] = *val++; msg[idx]='\0';
                                    print_term(msg);
                                    
                                    print_term("[ RING 3 - RAM FISICA APP ]");
                                    
                                    itoa(pfa_used_mb, num_buf);
                                    msg[0]='\0'; idx=0; label = "  Utilizzata (MB)    : ";
                                    while(*label) msg[idx++] = *label++;
                                    val = num_buf; while(*val) msg[idx++] = *val++; msg[idx]='\0';
                                    print_term(msg);
                                    
                                    itoa(pfa_free_mb, num_buf);
                                    msg[0]='\0'; idx=0; label = "  Libera (MB)        : ";
                                    while(*label) msg[idx++] = *label++;
                                    val = num_buf; while(*val) msg[idx++] = *val++; msg[idx]='\0';
                                    print_term(msg);
                                    print_term("=======================================");
                                }
                                // COMANDO SYSCALL (Testiamo l'Interrupt Descriptor Table)
                                else if (term_buffer[0] == 's' && term_buffer[1] == 'y' && term_buffer[2] == 's' && term_buffer[3] == 'c' && term_buffer[4] == 'a' && term_buffer[5] == 'l' && term_buffer[6] == 'l') {
                                    print_term("Lancio interruzione software 0x80 al processore...");
                                    
                                    // Chiamata Assembly pura che blocca la CPU e salta alla nostra IDT
                                    asm volatile ("int $0x80");
                                    
                                    print_term("Ritorno al Terminale avvenuto con successo.");
                                }
                                
                                
                                
                                // COMANDO RUN (Es: "run /editor.edxi" oppure comodamente "run editor.edxi")
                                else if (term_buffer[0] == 'r' && term_buffer[1] == 'u' && term_buffer[2] == 'n' && term_buffer[3] == ' ') {
                                    char target_file[64];
                                    int t_idx = 0; int idx = 4;
                                    
                                    // Se l'utente non ha messo lo slash iniziale, aggiungiamo solo '/'
                                    // in modo da cercare l'app nella Root, coerentemente con gli altri comandi.
                                    if (term_buffer[idx] != '/') {
                                        target_file[t_idx++] = '/';
                                    }
                                    
                                    while(term_buffer[idx] != '\0' && t_idx < 63) {
                                        target_file[t_idx++] = term_buffer[idx++];
                                    }
                                    target_file[t_idx] = '\0';

                                    int is_bin = 0;
                                    int is_edxi = 0;
                                    if (t_idx >= 4 && target_file[t_idx-4] == '.' && target_file[t_idx-3] == 'b' && target_file[t_idx-2] == 'i' && target_file[t_idx-1] == 'n') is_bin = 1;
                                    if (t_idx >= 5 && target_file[t_idx-5] == '.' && target_file[t_idx-4] == 'e' && target_file[t_idx-3] == 'd' && target_file[t_idx-2] == 'x' && target_file[t_idx-1] == 'i') is_edxi = 1;

                                    if (!is_bin && !is_edxi) {
                                        print_term("ERRORE: Formato non supportato. Usa .bin o .edxi");
                                    } 
                                    else {
                                        // Diamo 2 Megabyte di RAM all'app per girare e fare le malloc in comodità
                                        //int total_ram_for_app = 2048 * 1024;
                                        // 1. Creiamo la Page Table privata per questa App (4KB)
                                        uint32_t* app_pt = (uint32_t*)kmalloc(4096);

                                        if (app_pt) {
                                            // Puliamo la tabella (tutto non mappato)
                                            for(int k=0; k<1024; k++) app_pt[k] = 0;

                                            // 2. Leggiamo il file in un buffer temporaneo del Kernel
                                            uint32_t actual_size = 0;
                                            void* temp_buffer = kmalloc(2000000); // Alzato a 2 MB per reggere TCC!
                                            
                                            if (temp_buffer && ext2_read_file(target_file, (char*)temp_buffer, 2000000, &actual_size)) {
                                                
                                                // 3. Quanti frame fisici servono ESATTAMENTE per il codice dell'app?
                                                uint32_t frames_needed = (actual_size / 4096) + 1;
                                                
                                                // 4. Allochiamo e mappiamo solo lo stretto necessario!
                                                for(uint32_t f = 0; f < frames_needed; f++) {
                                                    uint32_t phys_frame = pfa_alloc_frame();
                                                    app_pt[f] = phys_frame | 0x07; // Present, R/W, User Ring 3
                                                    
                                                    // Copiamo i dati dal buffer temporaneo al frame fisico
                                                    uint8_t* dest = (uint8_t*)phys_frame; // Identity mapping ci permette di scriverci direttamente
                                                    uint8_t* src = (uint8_t*)temp_buffer + (f * 4096);
                                                    for(int b = 0; b < 4096; b++) {
                                                        if ((f * 4096 + b) < actual_size) {
                                                            dest[b] = src[b];
                                                        } else {
                                                            dest[b] = 0; // AZZERIAMO LA CODA DEL FRAME (Fondamentale per la .bss)
                                                        }
                                                    }
                                                }
                                                
                                                kfree(temp_buffer); // Finito! Liberiamo il buffer temporaneo

                                                // Puntiamo al primo frame fisico per leggere l'header EDXI
                                                uint8_t* mem_ptr = (uint8_t*)(app_pt[0] & 0xFFFFF000); 

                                                // === IL PARSER DELL'HEADER EDXI UNIFICATO ===
                                                int is_gui = 0;
                                                int is_virtual = 0; 
                                                uint32_t entry_offset = 0;
                                                
                                                if (is_edxi) {
                                                    if (mem_ptr[0] == 'E' && mem_ptr[1] == 'D' && mem_ptr[2] == 'X' && mem_ptr[3] == 'I') {
                                                        if (mem_ptr[4] == 1) is_gui = 1;
                                                        if (mem_ptr[5] == 1) is_virtual = 1; 
                                                        entry_offset = 16; 
                                                    } else {
                                                        print_term("ERRORE: File .edxi corrotto.");
                                                        // Se l'app fallisce, dobbiamo pulire subito!
                                                        for(uint32_t f = 0; f < frames_needed; f++) pfa_free_frame(app_pt[f] & 0xFFFFF000);
                                                        kfree(app_pt);
                                                        continue; 
                                                    }
                                                }
                                                
                                                if (!is_virtual || !is_edxi) {
                                                    print_term("SICUREZZA: App obsoleta (Ring 0). Esecuzione bloccata!");
                                                    for(uint32_t f = 0; f < frames_needed; f++) pfa_free_frame(app_pt[f] & 0xFFFFF000);
                                                    kfree(app_pt);
                                                    continue;
                                                }
                                                
                                                print_term("Lancio Eseguibile (Demand Paging Attivo)...");

                                                // Il Pointer in Assembly rimane lo stesso (L'offset virtuale fisso a 0x40000010)
                                                void (*app_func)(void) = (void (*)(void))(0x40000000 + entry_offset);
                                                
                                                int new_pid = get_free_pid();
                                                if (new_pid != -1) {
                                                    if (is_gui) {
                                                        int app_win_id = open_window(WIN_TYPE_APP_GUI, target_file, 150, 150, 400, 300, new_pid);
                                                        create_process(app_func, app_win_id, app_pt, is_virtual, top_win, new_pid); 
                                                    } else {
                                                        create_process(app_func, window_order[0], app_pt, is_virtual, top_win, new_pid);
                                                    }
                                                } else {
                                                    print_term("ERRORE: Raggiunto il limite massimo di processi attivi.");
                                                    for(uint32_t f = 0; f < frames_needed; f++) pfa_free_frame(app_pt[f] & 0xFFFFF000);
                                                    kfree(app_pt);
                                                }
                                            } else {
                                                print_term("Errore: Impossibile leggere il file o percorso errato.");
                                                if (temp_buffer) kfree(temp_buffer);
                                                kfree(app_pt);
                                            }
                                        } else {
                                            print_term("Errore: Impossibile creare la Page Table!");
                                        }
                                    }
                                }


                                // COMANDO PS (Lista i processi in esecuzione)
                                else if (term_buffer[0] == 'p' && term_buffer[1] == 's' && term_buffer[2] == '\0') {
                                    print_term("PID | WIN | STATO");
                                    print_term("-------------------");
                                    
                                    int found = 0;
                                    for (int p = 1; p < MAX_PROCESSES; p++) {
                                        if (process_table[p].active) {
                                            found = 1;
                                            char msg[64];
                                            
                                            // Assembliamo la stringa a mano per non usare librerie esterne
                                            msg[0] = ' '; itoa(p, msg + 1);
                                            int len = 0; while(msg[len]) len++;
                                            while(len < 6) msg[len++] = ' ';
                                            msg[len++] = '|'; msg[len++] = ' ';
                                            
                                            itoa(process_table[p].linked_window, msg + len);
                                            while(msg[len]) len++;
                                            while(len < 12) msg[len++] = ' ';
                                            msg[len++] = '|'; msg[len++] = ' ';
                                            
                                            if (process_table[p].waiting_for_input) {
                                                msg[len++]='S'; msg[len++]='L'; msg[len++]='E'; msg[len++]='E'; msg[len++]='P';
                                            } else {
                                                msg[len++]='R'; msg[len++]='U'; msg[len++]='N';
                                            }
                                            msg[len] = '\0';
                                            print_term(msg);
                                        }
                                    }
                                    if (!found) print_term("Nessuna app in background.");
                                }
                                
                                // COMANDO KILL (Es: "kill 2")
                                else if (term_buffer[0] == 'k' && term_buffer[1] == 'i' && term_buffer[2] == 'l' && term_buffer[3] == 'l' && term_buffer[4] == ' ') {
                                    int pid_to_kill = 0;
                                    int idx = 5;
                                    
                                    // Estraiamo il numero digitato dall'utente
                                    while (term_buffer[idx] >= '0' && term_buffer[idx] <= '9') {
                                        pid_to_kill = pid_to_kill * 10 + (term_buffer[idx] - '0');
                                        idx++;
                                    }
                                    
                                    // Se il processo esiste ed è un'app (PID > 0), lo annientiamo
                                    if (pid_to_kill > 0 && pid_to_kill < MAX_PROCESSES && process_table[pid_to_kill].active) {
                                        // Come ogni altra chiusura: anche le sue finestre
                                        // se ne vanno, e la taskbar torna a posto
                                        close_app(pid_to_kill);

                                        print_term("SUCCESSO: Processo terminato e RAM liberata.");
                                    } else {
                                        print_term("ERRORE: PID non valido o app non trovata.");
                                    }
                                }

                                
                                // COMANDO ALLOC (Testiamo l'allocazione di Blocchi e Inode)
                                else if (term_buffer[0] == 'a' && term_buffer[1] == 'l' && term_buffer[2] == 'l' && term_buffer[3] == 'o' && term_buffer[4] == 'c') {
                                    print_term("Ricerca di spazio e Inode liberi...");
                                    render_desktop(mouse_x, mouse_y, is_clicking); 
                                    
                                    uint32_t free_block = ext2_allocate_block();
                                    uint32_t free_inode = ext2_allocate_inode();
                                    
                                    if (free_block > 0 && free_inode > 0) {
                                        char msg1[64]; char num1[32];
                                        itoa(free_block, num1);
                                        msg1[0]='\0'; int idx=0; char* label1 = "-> Blocco Dati prenotato: n. ";
                                        while(*label1) msg1[idx++] = *label1++;
                                        char* val1 = num1; while(*val1) msg1[idx++] = *val1++; msg1[idx]='\0';
                                        print_term(msg1);

                                        char msg2[64]; char num2[32];
                                        itoa(free_inode, num2);
                                        msg2[0]='\0'; idx=0; char* label2 = "-> Inode prenotato: n. ";
                                        while(*label2) msg2[idx++] = *label2++;
                                        char* val2 = num2; while(*val2) msg2[idx++] = *val2++; msg2[idx]='\0';
                                        print_term(msg2);
                                        
                                        print_term("Mappe del disco aggiornate permanentemente.");
                                    } else {
                                        print_term("ERRORE: Disco pieno o corrotto.");
                                    }
                                }
                                
                                // COMANDO CREATE (Es: "create /users/admin/docs/ciao.txt testo")
                                else if (term_buffer[0] == 'c' && term_buffer[1] == 'r' && term_buffer[2] == 'e' && term_buffer[3] == 'a' && term_buffer[4] == 't' && term_buffer[5] == 'e' && term_buffer[6] == ' ') {
                                    char full_file[64];
                                    char filedata[256];
                                    int idx = 7;
                                    int f_idx = 0;
                                    
                                    if (term_buffer[idx] != '/') full_file[f_idx++] = '/';
                                    while (term_buffer[idx] != ' ' && term_buffer[idx] != '\0' && f_idx < 63) {
                                        full_file[f_idx++] = term_buffer[idx++];
                                    }
                                    full_file[f_idx] = '\0';
                                    
                                    if (term_buffer[idx] == ' ') idx++; 
                                    
                                    int d_idx = 0;
                                    while (term_buffer[idx] != '\0' && d_idx < 255) {
                                        filedata[d_idx++] = term_buffer[idx++];
                                    }
                                    filedata[d_idx] = '\0';
                                    
                                    if (f_idx > 1 && d_idx > 0) {
                                        print_term("Creazione file in corso...");
                                        render_desktop(mouse_x, mouse_y, is_clicking);
                                        
                                        int create_res = ext2_write_file(full_file, filedata, d_idx);
                                        if (create_res == 1) {
                                            print_term("SUCCESSO! File creato.");
                                        } else if (create_res == -1) {
                                            print_term("ERRORE: Nome duplicato in questa cartella!");
                                        } else if (create_res == -2) {
                                            print_term("ERRORE: La cartella madre non esiste!");
                                        } else {
                                            print_term("ERRORE: Impossibile creare il file.");
                                        }
                                    } else {
                                        print_term("Sintassi: create /percorso/nome.txt testodelinserire");
                                    }
                                }
                                
                                // COMANDO MKDIR (Es: "mkdir /users/admin")
                                else if (term_buffer[0] == 'm' && term_buffer[1] == 'k' && term_buffer[2] == 'd' && term_buffer[3] == 'i' && term_buffer[4] == 'r' && term_buffer[5] == ' ') {
                                    char full_dir[64];
                                    int idx = 6;
                                    int f_idx = 0;
                                    
                                    // Se l'utente non ha messo lo slash, lo aggiungiamo noi (crea nella Root)
                                    if (term_buffer[idx] != '/') {
                                        full_dir[f_idx++] = '/';
                                    }
                                    
                                    while (term_buffer[idx] != ' ' && term_buffer[idx] != '\0' && f_idx < 63) {
                                        full_dir[f_idx++] = term_buffer[idx++];
                                    }
                                    full_dir[f_idx] = '\0';
                                    
                                    if (f_idx > 1) { // 1 perché c'è sempre almeno lo '/'
                                        print_term("Creazione directory in corso...");
                                        render_desktop(mouse_x, mouse_y, is_clicking);
                                        
                                        int mkdir_res = ext2_make_directory(full_dir);
                                        if (mkdir_res == 1) {
                                            print_term("SUCCESSO! Cartella creata. Digita 'ls' per vederla.");
                                        } else if (mkdir_res == -1) {
                                            print_term("ERRORE: Un file o cartella con questo nome esiste gia'!");
                                        } else if (mkdir_res == -2) {
                                            print_term("ERRORE: La cartella madre non esiste nel percorso specificato!");
                                        } else {
                                            print_term("ERRORE: Impossibile creare la cartella.");
                                        }
                                    } else {
                                        print_term("Sintassi: mkdir /percorso/nomecartella");
                                    }
                                }

                                // COMANDO RM (Es: "rm /users/admin/docs/test.txt")
                                else if (term_buffer[0] == 'r' && term_buffer[1] == 'm' && term_buffer[2] == ' ') {
                                    char full_file[64];
                                    int idx = 3;
                                    int f_idx = 0;
                                    
                                    if (term_buffer[idx] != '/') full_file[f_idx++] = '/';
                                    while (term_buffer[idx] != ' ' && term_buffer[idx] != '\0' && f_idx < 63) {
                                        full_file[f_idx++] = term_buffer[idx++];
                                    }
                                    full_file[f_idx] = '\0';
                                    
                                    if (f_idx > 1) {
                                        print_term("Eliminazione file in corso...");
                                        render_desktop(mouse_x, mouse_y, is_clicking);
                                        
                                        int rm_result = ext2_delete_file(full_file);
                                        
                                        if (rm_result == 1) {
                                            print_term("SUCCESSO! File e spazio riciclato correttamente.");
                                        } else if (rm_result == -1) {
                                            print_term("ERRORE: Impossibile eliminare una directory con 'rm'.");
                                        } else {
                                            print_term("ERRORE: File non trovato o percorso errato.");
                                        }
                                    } else {
                                        print_term("Sintassi: rm /percorso/nomefile.txt");
                                    }
                                }

                                // COMANDO RMDIR (Es: "rmdir /cartella/sottocartella")
                                else if (term_buffer[0] == 'r' && term_buffer[1] == 'm' && term_buffer[2] == 'd' && term_buffer[3] == 'i' && term_buffer[4] == 'r' && term_buffer[5] == ' ') {
                                    int idx = 6;
                                    int f_idx = 0;
                                    
                                    // Aggiungiamo lo slash alla radice se manca
                                    if (term_buffer[idx] != '/') pending_rmdir_path[f_idx++] = '/';
                                    
                                    while (term_buffer[idx] != ' ' && term_buffer[idx] != '\0' && f_idx < 63) {
                                        pending_rmdir_path[f_idx++] = term_buffer[idx++];
                                    }
                                    pending_rmdir_path[f_idx] = '\0';
                                    
                                    // Se ha digitato almeno una lettera oltre allo slash (per non fargli cancellare la root '/')
                                    if (f_idx > 1) {
                                        pending_rmdir = 1; // Attiviamo lo stato di allerta
                                        print_term("ATTENZIONE: Sei sicuro di voler eliminare la cartella");
                                        print_term("e TUTTO il suo contenuto ricorsivamente? (y/n)");
                                    } else {
                                        print_term("Sintassi: rmdir /percorso/nomecartella");
                                    }
                                }

                                
                                // COMANDO CP (Copia file) Es: "cp /cartella/file.txt /dest"
                                else if (term_buffer[0] == 'c' && term_buffer[1] == 'p' && term_buffer[2] == ' ') {
                                    char src_path[128];
                                    char dest_dir[128];
                                    char filename[64];
                                    int idx = 3;
                                    int s_idx = 0, d_idx = 0, f_idx = 0;

                                    // 1. Estraiamo il percorso del file sorgente
                                    if (term_buffer[idx] != '/') src_path[s_idx++] = '/';
                                    while (term_buffer[idx] != ' ' && term_buffer[idx] != '\0' && s_idx < 127) {
                                        src_path[s_idx++] = term_buffer[idx++];
                                    }
                                    src_path[s_idx] = '\0';

                                    // 2. Estraiamo la cartella di destinazione
                                    if (term_buffer[idx] == ' ') idx++;
                                    if (term_buffer[idx] != '/') dest_dir[d_idx++] = '/';
                                    while (term_buffer[idx] != ' ' && term_buffer[idx] != '\0' && d_idx < 127) {
                                        dest_dir[d_idx++] = term_buffer[idx++];
                                    }
                                    dest_dir[d_idx] = '\0';

                                    if (s_idx > 1 && d_idx > 1) {
                                        // 3. Estraiamo solo il NOME del file dalla sorgente (es: "logo.bmp")
                                        int last_slash = -1;
                                        for (int i = 0; i < s_idx; i++) {
                                            if (src_path[i] == '/') last_slash = i;
                                        }
                                        for (int i = last_slash + 1; i < s_idx; i++) {
                                            filename[f_idx++] = src_path[i];
                                        }
                                        filename[f_idx] = '\0';

                                        // 4. Creiamo il percorso di destinazione finale
                                        char final_dest[256];
                                        int fd_idx = 0;
                                        for (int i = 0; i < d_idx; i++) final_dest[fd_idx++] = dest_dir[i];
                                        if (final_dest[fd_idx - 1] != '/') final_dest[fd_idx++] = '/';
                                        for (int i = 0; i < f_idx; i++) final_dest[fd_idx++] = filename[i];
                                        final_dest[fd_idx] = '\0';

                                        print_term("Inizializzazione copia in corso...");
                                        // Stampiamo le variabili per controllare che il parser funzioni
                                        char msg_src[150] = "Da: ";
                                        int m_idx = 4; for(int i=0; src_path[i]; i++) msg_src[m_idx++] = src_path[i]; msg_src[m_idx] = '\0';
                                        print_term(msg_src);
                                        
                                        char msg_dst[150] = "A:  ";
                                        m_idx = 4; for(int i=0; final_dest[i]; i++) msg_dst[m_idx++] = final_dest[i]; msg_dst[m_idx] = '\0';
                                        print_term(msg_dst);
                                        
                                        render_desktop(mouse_x, mouse_y, is_clicking);

                                        int copy_res = ext2_copy_file(src_path, final_dest);
                                        
                                        // Forziamo un ridisegno per pulire lo schermo dalla barra scomparsa
                                        render_desktop(mouse_x, mouse_y, is_clicking); 
                                        
                                        // Traduciamo i codici di errore in messaggi testuali per l'utente
                                        if (copy_res == 1) print_term("SUCCESSO! File copiato.");
                                        else if (copy_res == -1) print_term("ERRORE: File di destinazione gia' esistente.");
                                        else if (copy_res == -2) print_term("ERRORE: Cartella di destinazione inesistente.");
                                        else if (copy_res == -3) print_term("ERRORE: File sorgente inesistente.");
                                        else if (copy_res == -4) print_term("ERRORE: File troppo grande per la RAM (Max 10MB).");
                                        else if (copy_res == -5) print_term("ERRORE: RAM del Kernel insufficiente.");
                                        else if (copy_res == -6) print_term("ERRORE: Disco attualmente occupato. Riprova.");
                                        else print_term("ERRORE: Scrittura fallita (Disco pieno o corrotto).");
                                        
                                    } else {
                                        print_term("Sintassi: cp /sorgente/file.txt /destinazione");
                                    }
                                }


                                // COMANDO INSMOD (Carica un Modulo Driver) Es: "insmod /system/drivers/test.drv"
                                
                                else if (term_buffer[0] == 'i' && term_buffer[1] == 'n' && term_buffer[2] == 's' && term_buffer[3] == 'm' && term_buffer[4] == 'o' && term_buffer[5] == 'd') {
                                    
                                    if (term_buffer[6] == ' ') {
                                        char drv_path[64];
                                        int d_idx = 0; int idx = 7;
                                        
                                        if (term_buffer[idx] != '/') drv_path[d_idx++] = '/';
                                        while(term_buffer[idx] != '\0' && term_buffer[idx] != ' ' && d_idx < 63) {
                                            drv_path[d_idx++] = term_buffer[idx++];
                                        }
                                        drv_path[d_idx] = '\0';
                                        
                                        print_term("Caricamento Modulo Kernel in corso...");
                                        render_desktop(mouse_x, mouse_y, is_clicking);
                                        
                                        int res = load_driver(drv_path);
                                        
                                        if (res == 1) print_term("SUCCESSO! Audio in riproduzione in background!");
                                        else if (res == 10) print_term("SB16 ERRORE: Scheda non rilevata o spenta.");
                                        else if (res == 11) print_term("SB16 ERRORE: Memoria RAM DMA esaurita.");
                                        else if (res == 12) print_term("SB16 ERRORE: Il file /startup.wav non esiste!");
                                        else if (res == 13) print_term("SB16 ERRORE: Il file audio e' corrotto o vuoto.");
                                        else if (res == 14) print_term("SB16 ERRORE: Formato errato. Richiesto WAV 8-bit Mono!");
                                        else if (res == 15) print_term("SB16 ERRORE: Nessun dato audio trovato nel file.");
                                        else if (res == 0) print_term("Modulo eseguito e scaricato correttamente dalla RAM.");
                                        else if (res == -1) print_term("ERRORE: Modulo .drv non trovato sul disco.");
                                        else if (res == -3) print_term("ERRORE: Memoria Kernel insufficiente.");
                                        else print_term("ERRORE FATALE: Il driver ha fallito l'inizializzazione.");
                                    } else {
                                        print_term("Sintassi: insmod /system/drivers/nome.drv");
                                    }
                                }

                                //AGGIUNGI QUI SOPRA I NUOVI COMANDI SE SERVE =!=!=!=!=!=!=!=!=!=!=!=!=!=!=!=!=!=!=!=
                                else if (term_len > 0) {
                                    print_term("Errore: Comando non trovato.");
                                }
                                
                                term_len = 0; 
                                term_cursor = 0; // AGGIUNTO IL RESET CURSORE
                            }
                            else if (ascii >= 32 && ascii <= 126 && term_len < 254) { // MODIFICATA QUESTA CONDIZIONE
                                // Spostiamo a destra tutte le lettere successive
                                for (int k = term_len; k >= term_cursor; k--) {
                                    term_buffer[k + 1] = term_buffer[k];
                                }
                                term_buffer[term_cursor] = ascii; // Inseriamo la lettera
                                term_cursor++;
                                term_len++;
                            }
                            
                            blink_counter = 0;
                            render_desktop(mouse_x, mouse_y, is_clicking);
                        }
                    } 
                }
            }
        }
        read_rtc(); 
        if (rtc_second != last_second) {
            last_second = rtc_second; // Aggiorniamo la memoria
            // L'orologio è scattato: Ridisegna
            refresh_requested = 1;
            // è passato un secondo! Forziamo un ridisegno per aggiornare l'orologio
            
        }
    }
} 
