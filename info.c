// Third-Millennium Operating System - System Info App
// Copyright (C) 2026  Alberto Sanfelice

// COMPILAZIONE + CONVERSIONE + INSTALLAZIONE
// i686-elf-gcc -m32 -ffreestanding -fno-pie -fno-pic -nostdlib -O0 -c antem_libc/stdio.c -o stdio.o
// i686-elf-gcc -m32 -ffreestanding -fno-pie -fno-pic -nostdlib -O0 -c antem_libc/stdlib.c -o stdlib.o
// i686-elf-gcc -m32 -ffreestanding -fno-pie -fno-pic -nostdlib -O0 -c antem_libc/string.c -o string.o
// i686-elf-gcc -m32 -ffreestanding -fno-pie -fno-pic -nostdlib -O0 -I./antem_libc -c info.c -o info.o


// i686-elf-ld -T app_linker.ld info.o stdio.o stdlib.o string.o -o info.bin
// cat header_gui.bin info.bin > info.edxi 
// e2cp info.edxi disk.img:/system/sys_apps/info.edxi 


#include "antem_libc/stdio.h"
#include "antem_libc/stdlib.h"

int main_win = 0;
int credits_win = -1;       // finestra dei contributori, -1 = chiusa
int info_line = 0;          // 0 = autore, 2 = guida, 3 = sito

void cmd_close() {
    exit(0);
}

// Apre la finestra figlio con l'elenco dei contributori. Il testo si scrive
// una volta sola, all'apertura, nel contesto della finestra figlio: ogni
// finestra ha le sue aree di testo, e l'indice 0 della figlia non e' quello
// della madre.
void cmd_contributors() {
    if (credits_win != -1) return;      // gia' aperta

    credits_win = gui_spawn_window("Contributors", 320, 170);
    if (credits_win == -1) return;

    gui_set_context(credits_win);
    gui_textarea_set(0,
        "Andrea Simonetti: SplitMix32 for random numbers, the operating system "
        "name change from Ante-Millennium OS to Third-Millennium OS, and other "
        "great ideas.");
    gui_set_context(main_win);
}

void cmd_user_guide() {
    info_line = (info_line == 2) ? 0 : 2;
}

void cmd_website() {
    info_line = (info_line == 3) ? 0 : 3;
}

menu_item_t credits_items[] = {
    { "See contributors", cmd_contributors }
};

menu_item_t help_items[] = {
    { "User guide", cmd_user_guide },
    { "Website",    cmd_website    }
};

void main() {
    gui_set_window_size(250, 180);

    gui_set_window_min_size(230, 170);  // sotto questa misura non si stringe

    while(1) {
        gui_poll_events();

        // Se l'utente ha chiuso la finestra dei contributori, la si dimentica:
        // cosi' il menu la potra' riaprire
        if (credits_win != -1 && gui_is_window_open(credits_win) == 0) {
            credits_win = -1;
        }

        // =======================================================
        // FINESTRA PRINCIPALE
        // =======================================================
        gui_set_context(main_win);
        gui_clear();

        // L'ordine delle chiamate decide anche l'ordine sulla barra:
        // il primo gui_menu prende lo slot 0, il secondo lo slot 1.
        gui_menu("Credits", credits_items, 1);
        gui_menu("Help",    help_items,    2);

        gui_text_bold(ANCHOR_TC, 0, 20, "Third-Millennium", 0xFF000000);
        gui_text_bold(ANCHOR_TC, 0, 40, "Alpha", 0xFF000000);

        if (info_line == 2)
            gui_text_bold(ANCHOR_TC, 0, 80, "Guide: not available yet", 0xFF000000);
        else if (info_line == 3)
            gui_text_bold(ANCHOR_TC, 0, 80, "5anfelice.it", 0xFF000000);
        else
            gui_text_bold(ANCHOR_TC, 0, 80, "Developed by Alberto Sanfelice", 0xFF000000);

        gui_action_button(ANCHOR_BC, 0, 10, 150, 25, BUTTON_DEFAULT, 0xFF000000, "Close", cmd_close);

        // =======================================================
        // FINESTRA DEI CONTRIBUTORI
        // Misure scelte perche' il ridimensionamento automatico sui contenuti
        // non la allarghi: 10 + 286 + 24 = 320, 10 + 110 + 40 = 160 < 170
        // =======================================================
        if (credits_win != -1) {
            gui_set_context(credits_win);
            gui_clear();
            gui_textarea(ANCHOR_TL, 10, 10, 286, 110, 0xFFF4F4F4, 0, TEXTAREA_READONLY);
        }

        gui_yield();
    }
}
