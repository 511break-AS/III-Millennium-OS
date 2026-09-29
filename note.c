// Third-Millennium Operating System - Note
// Copyright (C) 2026  Alberto Sanfelice

// COMPILAZIONE + CONVERSIONE + INSTALLAZIONE
// i686-elf-gcc -m32 -ffreestanding -fno-pie -fno-pic -nostdlib -O0 -c antem_libc/stdio.c -o stdio.o
// i686-elf-gcc -m32 -ffreestanding -fno-pie -fno-pic -nostdlib -O0 -c antem_libc/stdlib.c -o stdlib.o
// i686-elf-gcc -m32 -ffreestanding -fno-pie -fno-pic -nostdlib -O0 -c antem_libc/string.c -o string.o
// i686-elf-gcc -m32 -ffreestanding -fno-pie -fno-pic -nostdlib -O0 -I./antem_libc -c note.c -o note.o
// i686-elf-ld -T app_linker.ld note.o stdio.o stdlib.o string.o -o note.bin
// cat header_gui.bin note.bin > note.edxi
// e2cp note.edxi disk.img:/apps/note.edxi

#include "antem_libc/stdio.h"
#include "antem_libc/stdlib.h"

int main_win = 0;

#define MARGINE 8

void main() {
    gui_set_window_size(480, 360);
    gui_set_window_min_size(260, 180);

    while (1) {
        gui_poll_events();
        gui_set_context(main_win);
        gui_clear();

        // L'area di testo riempie la finestra, qualunque misura abbia.
        // E' ancorata in basso a destra di proposito: il kernel non conta gli
        // elementi ancorati a destra e in basso nel calcolo della misura minima,
        // altrimenti il minimo inseguirebbe la finestra e non si potrebbe piu'
        // stringerla. Lo spazio utile e' la finestra meno 8 pixel di bordi in
        // larghezza e 34 in altezza (barra del titolo compresa).
        int ww = 0, wh = 0;
        get_window_size(&ww, &wh);
        int w = (ww - 8)  - 2 * MARGINE;
        int h = (wh - 34) - 2 * MARGINE;
        if (w < 40) w = 40;
        if (h < 40) h = 40;

        gui_textarea(ANCHOR_BR, MARGINE, MARGINE, w, h, 0xFFFFFFFF, 0, TEXTAREA_EDIT);

        gui_yield();
    }
}
