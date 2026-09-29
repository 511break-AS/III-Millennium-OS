// Third-Millennium Operating System - antem_stdlib
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

// antem_libc/stdlib.c

// CONTRIB: 
// Andrea Simonetti - > SplitMix32 for random numbers



#include "stdlib.h"
#include "stddef.h"
#include "string.h"
#include "time.h"

int errno = 0; // LA NOSTRA VARIABILE GLOBALE PER GLI ERRORI


// --- MAPPA DELLA MEMORIA DELL'APP ---
// Il codice delle app occupa i primi KB. Fissiamo l'inizio dell'Heap 
// al traguardo di 1 MegaByte (0x40100000). Così c'è spazio per app enormi!
#define HEAP_START 0x40100000
#define HEAP_SIZE  (2000 * 1024) // 2 MB di Heap dinamico gestito dal Demand Paging

typedef struct malloc_block {
    size_t size;
    int is_free;
    struct malloc_block* next;
} malloc_block_t;

malloc_block_t* user_heap_head = (malloc_block_t*)HEAP_START;
int heap_initialized = 0;

void init_heap_user() {
    user_heap_head->size = HEAP_SIZE - sizeof(malloc_block_t);
    user_heap_head->is_free = 1;
    user_heap_head->next = 0; 
    heap_initialized = 1;
}



void* malloc(size_t size) {
    // Inizializzazione automatica "pigra" (Solo la primissima volta)
    if (!heap_initialized) init_heap_user();

    // Arrotondiamo la dimensione richiesta ai 4 byte più vicini (Allineamento a 32-bit)
    if (size % 4 != 0) size = size + (4 - (size % 4));

    malloc_block_t* current = user_heap_head;
    
    // Scorriamo la lista per trovare il primo buco libero abbastanza grande
    while (current != 0) {
        if (current->is_free && current->size >= size) {
            
            // Se il buco è molto più grande di quello che ci serve, lo spacchiamo a metà
            if (current->size > size + sizeof(malloc_block_t) + 4) {
                malloc_block_t* new_block = (malloc_block_t*)((char*)current + sizeof(malloc_block_t) + size);
                new_block->size = current->size - size - sizeof(malloc_block_t);
                new_block->is_free = 1;
                new_block->next = current->next;
                
                current->size = size;
                current->next = new_block;
            }
            
            current->is_free = 0; // Segniamo il blocco come occupato
            // Ritorniamo l'indirizzo *dopo* l'intestazione segreta!
            return (void*)((char*)current + sizeof(malloc_block_t));
        }
        current = current->next;
    }
    
    return 0; // Memoria esaurita
}

void free(void* ptr) {
    if (!ptr) return;
    
    // Facciamo un passo indietro per leggere l'intestazione segreta
    malloc_block_t* block = (malloc_block_t*)((char*)ptr - sizeof(malloc_block_t));
    block->is_free = 1; // Lo marchiamo come libero
    
    // DEFRAMMENTAZIONE: Uniamo i buchi adiacenti per evitare di frammentare la RAM
    malloc_block_t* current = user_heap_head;
    while (current != 0) {
        if (current->is_free && current->next != 0 && current->next->is_free) {
            // Fondiamo il blocco corrente col successivo
            current->size += sizeof(malloc_block_t) + current->next->size;
            current->next = current->next->next;
        } else {
            current = current->next;
        }
    }
}

// Converte una stringa ASCII in un numero intero (Gestisce anche i negativi)
int atoi(const char* str) {
    int res = 0;
    int sign = 1;
    int i = 0;
    
    if (str[0] == '-') {
        sign = -1;
        i++;
    }
    
    for (; str[i] != '\0'; ++i) {
        if (str[i] >= '0' && str[i] <= '9') {
            res = res * 10 + str[i] - '0';
        } else {
            break; // Se incontra una lettera, si ferma
        }
    }
    return sign * res;
}


void* realloc(void* ptr, size_t size) {
    // Regole standard C
    if (size == 0) { free(ptr); return NULL; }
    if (!ptr) return malloc(size);

    // Facciamo un passo indietro per leggere quanto era grande il vecchio blocco
    malloc_block_t* block = (malloc_block_t*)((char*)ptr - sizeof(malloc_block_t));
    
    // Se il blocco attuale è già grande abbastanza, non facciamo nulla! (Ottimizzazione)
    if (block->size >= size) return ptr;

    // Altrimenti allochiamo un nuovo spazio più grande
    void* new_ptr = malloc(size);
    if (!new_ptr) return NULL; // Memoria esaurita

    // Copiamo i vecchi dati (usando la tua nuova memcpy!) e liberiamo il vecchio blocco
    memcpy(new_ptr, ptr, block->size);
    free(ptr);
    
    return new_ptr;
}


// ==========================================
// GENERATORE NUMERI CASUALI (SplitMix32)
// ==========================================
static unsigned int rand_state = 1;

int rand(void) {
    unsigned int z = (rand_state += 0x9e3779b9);
    z = (z ^ (z >> 15)) * 0x85ebca6b;
    z = z ^ (z >> 13);
    
    // Mascheriamo il bit più significativo (segno) per garantire
    // che il ritorno sia un int sempre positivo compreso tra 0 e RAND_MAX,
    // rispettando al 100% lo standard C.
    return (int)(z & 0x7FFFFFFF);
}

void srand(unsigned int seed) {
    rand_state = seed;
    rand(); // Consumiamo il primo numero per rimescolare subito lo stato
}


// Termina immediatamente il processo invocando la Syscall 13
void exit(int status) {
    asm volatile ("int $0x80" : : "a"(13));
    while(1); // Dice al compilatore: "Da qui non si torna vivi!"
}


/*
// ==========================================
// TCC WRAPPERS (Funzioni fittizie per il Linker)
// ==========================================
unsigned long int strtoul(const char *nptr, char **endptr, int base) { return 0; }
double strtod(const char *nptr, char **endptr) { return 0; }
long int strtol(const char *nptr, char **endptr, int base) { return 0; }
void qsort(void *base, size_t nmemb, size_t size, int (*compar)(const void *, const void *)) { }
char *getenv(const char *name) { return NULL; }

// Mock di math.h (Lo mettiamo qui così va a finire dentro stdlib.o)
double ldexp(double x, int exp) { return x; }

// Mock per localtime
struct tm *localtime(const time_t *timer) { 
    static struct tm dummy_time = {0}; 
    return &dummy_time; 
}

int mprotect(void *addr, size_t len, int prot) { return 0; }

int sigemptyset(void *set) { return 0; }
int sigaction(int signum, const void *act, void *oldact) { return 0; }

unsigned long long int strtoull(const char *nptr, char **endptr, int base) { return 0; }
long long int strtoll(const char *nptr, char **endptr, int base) { return 0; }
int close(int fd) { return 0; }
int execvp(const char *file, char *const argv[]) { return -1; }

*/