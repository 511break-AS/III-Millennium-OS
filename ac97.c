// Third-Millennium Operating System - Driver AC97
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

// COMPILAZIONE E INSTALLAZIONE (stessa ricetta di tutti i driver):
// i686-elf-gcc -m32 -O2 -fPIE -ffreestanding -fno-stack-protector -fno-asynchronous-unwind-tables -ffunction-sections -nostdlib -c ac97.c -o ac97.o
// i686-elf-ld -m elf_i386 -T driver.ld -o ac97.elf ac97.o
// i686-elf-objcopy -O binary ac97.elf ac97.drv
// e2cp ac97.drv disk.img:/system/drivers/ac97.drv
//
// Compilato con -fPIE il driver funziona a qualunque indirizzo il kernel lo
// carichi: ogni accesso a variabili, testi e funzioni e' calcolato rispetto
// alla posizione reale del codice. Non servono piu' ne' la rilocazione a mano
// dei puntatori ne' il trucco di inizializzare le variabili a 1.

// ========================================================
// DRIVER AUDIO: Intel AC'97 (Demone di Sistema Residente)
// ========================================================

// Deve coincidere campo per campo con kernel_api_t del kernel.
// I campi nuovi si aggiungono sempre in coda.
typedef struct {
    void (*print_term)(const char*);
    void* (*kmalloc)(unsigned int);
    void (*kfree)(void*);
    void (*outb)(unsigned short, unsigned char);
    unsigned char (*inb)(unsigned short);
    void (*outw)(unsigned short, unsigned short);
    unsigned short (*inw)(unsigned short);
    void* (*kmalloc_dma)(unsigned int);
    int (*ext2_read_file)(const char*, char*, int, unsigned int*);
    void (*kfree_dma)(void*);
    void (*register_audio)(void*);
    void (*register_net)(void*);
} kernel_api_t;

typedef struct {
    unsigned int pointer;
    unsigned short length;
    unsigned short flags;
} __attribute__((packed)) ac97_bdl_t;

// =====================================================================
// MEMORIA DEL DRIVER
// Ora vive davvero dentro il driver, non vicino all'indirizzo zero.
// =====================================================================
static char*           current_audio_ram = 0;
static ac97_bdl_t*     current_bdl = 0;
static unsigned short  nam_port_global = 0;
static unsigned short  nabm_port_global = 0;
static kernel_api_t*   kapi = 0;

// =====================================================================
// FUNZIONI DI SUPPORTO
// =====================================================================
static inline void outl(unsigned short port, unsigned int val) {
    __asm__ volatile("outl %0, %1" : : "a"(val), "Nd"(port));
}
static inline unsigned int inl(unsigned short port) {
    unsigned int ret;
    __asm__ volatile("inl %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}
static unsigned int pci_read_dword(unsigned char bus, unsigned char slot, unsigned char func, unsigned char offset) {
    unsigned int address = (unsigned int)((bus << 16) | (slot << 11) | (func << 8) | (offset & 0xFC) | 0x80000000u);
    outl(0xCF8, address);
    return inl(0xCFC);
}
static void pci_write_dword(unsigned char bus, unsigned char slot, unsigned char func, unsigned char offset, unsigned int val) {
    unsigned int address = (unsigned int)((bus << 16) | (slot << 11) | (func << 8) | (offset & 0xFC) | 0x80000000u);
    outl(0xCF8, address);
    outl(0xCFC, val);
}

// =====================================================================
// FUNZIONE DI RIPRODUZIONE (Chiamata su richiesta del Kernel/App)
// =====================================================================
static int ac97_play_file(const char* path) {
    if (!kapi || nabm_port_global == 0) return -1;

    kapi->outb(nabm_port_global + 0x1B, 0x02); // Reset DMA PCM Out
    for (volatile int i = 0; i < 50000; i++);  // Pausa per l'hardware

    if (current_audio_ram) { kapi->kfree(current_audio_ram); current_audio_ram = 0; }
    if (current_bdl) { kapi->kfree(current_bdl); current_bdl = 0; }

    unsigned int file_size = 0;
    kapi->ext2_read_file(path, 0, 0, &file_size);
    if (file_size == 0) return 0;

    current_audio_ram = (char*)kapi->kmalloc(file_size);
    if (!current_audio_ram) return 0;

    kapi->ext2_read_file(path, current_audio_ram, file_size, &file_size);

    int data_offset = -1;
    unsigned int data_size = 0;
    int offset = 12;
    while (offset < (int)file_size - 8) {
        char id0 = current_audio_ram[offset];     char id1 = current_audio_ram[offset + 1];
        char id2 = current_audio_ram[offset + 2]; char id3 = current_audio_ram[offset + 3];
        unsigned int chunk_size = *(unsigned int*)(current_audio_ram + offset + 4);

        if (id0 == 'd' && id1 == 'a' && id2 == 't' && id3 == 'a') {
            data_offset = offset + 8;
            data_size = chunk_size;
            break;
        }
        unsigned int skip = chunk_size;
        if (skip % 2 != 0) skip++;
        offset += 8 + skip;
    }

    if (data_offset == -1) {
        kapi->kfree(current_audio_ram); current_audio_ram = 0;
        return 0;
    }

    current_bdl = (ac97_bdl_t*)kapi->kmalloc(32 * sizeof(ac97_bdl_t));
    if (!current_bdl) {
        kapi->kfree(current_audio_ram); current_audio_ram = 0;
        return 0;
    }

    char* raw_audio = current_audio_ram + data_offset;
    unsigned int max_chunk_bytes = 64000;
    unsigned int bytes_processed = 0;
    int bdl_index = 0;

    while (bytes_processed < data_size && bdl_index < 32) {
        unsigned int chunk = data_size - bytes_processed;
        if (chunk > max_chunk_bytes) chunk = max_chunk_bytes;
        chunk = chunk - (chunk % 4);

        current_bdl[bdl_index].pointer = (unsigned int)(raw_audio + bytes_processed);
        current_bdl[bdl_index].length = chunk / 2;
        current_bdl[bdl_index].flags = 0;

        bytes_processed += chunk;
        bdl_index++;
    }

    current_bdl[bdl_index - 1].flags = 0x4000;

    outl(nabm_port_global + 0x10, (unsigned int)current_bdl);
    kapi->outb(nabm_port_global + 0x15, bdl_index - 1);
    kapi->outb(nabm_port_global + 0x1B, 0x01); // PLAY!

    return 1;
}

// =====================================================================
// FUNZIONE INIZIALE: il kernel salta qui, al primo byte del driver
// =====================================================================
int _start(kernel_api_t* api, void* my_base_address) {
    (void)my_base_address;   // non serve piu': -fPIE fa tutto da solo
    kapi = api;

    // 1. RICERCA SCHEDA PCI
    unsigned char ac97_bus = 0, ac97_slot = 0;
    int found = 0;
    for (int b = 0; b < 256; b++) {
        for (int s = 0; s < 32; s++) {
            if (pci_read_dword(b, s, 0, 0) == 0x24158086u) {
                ac97_bus = b; ac97_slot = s; found = 1; break;
            }
        }
        if (found) break;
    }

    if (!found) return -1;

    unsigned int bar0 = pci_read_dword(ac97_bus, ac97_slot, 0, 0x10);
    unsigned int bar1 = pci_read_dword(ac97_bus, ac97_slot, 0, 0x14);
    nam_port_global = bar0 & 0xFFFE;
    nabm_port_global = bar1 & 0xFFFE;

    if (nam_port_global == 0 || nabm_port_global == 0) return -1;

    // 2. RESET HARDWARE AC97
    api->outb(nabm_port_global + 0x1B, 0x02);
    unsigned int cmd_reg = pci_read_dword(ac97_bus, ac97_slot, 0, 0x04);
    pci_write_dword(ac97_bus, ac97_slot, 0, 0x04, cmd_reg | 0x0005);

    api->outw(nam_port_global + 0x00, 1);
    for (volatile int i = 0; i < 500000; i++);
    api->outw(nam_port_global + 0x02, 0x0000);
    api->outw(nam_port_global + 0x18, 0x0000);

    // 3. Registrazione presso il kernel. Con -fPIE l'indirizzo della funzione
    //    e' gia' quello vero: nessuna somma a mano. L'assegnazione avviene qui,
    //    a runtime, e mai in un inizializzatore statico.
    api->register_audio((void*)ac97_play_file);

    api->print_term("AC97: Servizio Audio installato e pronto.");

    return 1; // Restiamo in memoria per sempre!
}
