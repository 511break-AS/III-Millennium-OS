// Third-Millennium Operating System - Robot (chat con l'intelligenza artificiale)
// Copyright (C) 2026  Alberto Sanfelice

// Il permesso di parlare con l'IA non e' un'impostazione: e' il robottino.
// Finche' non lo trascini sul vassoio nella barra del titolo di questa
// finestra, il pulsante Invia non fa nulla. Se lo porti via a meta' di una
// risposta, la risposta si interrompe.

// COMPILAZIONE + CONVERSIONE + INSTALLAZIONE
// i686-elf-gcc -m32 -ffreestanding -fno-pie -fno-pic -nostdlib -O0 -c antem_libc/stdio.c -o stdio.o
// i686-elf-gcc -m32 -ffreestanding -fno-pie -fno-pic -nostdlib -O0 -c antem_libc/stdlib.c -o stdlib.o
// i686-elf-gcc -m32 -ffreestanding -fno-pie -fno-pic -nostdlib -O0 -c antem_libc/string.c -o string.o
// i686-elf-gcc -m32 -ffreestanding -fno-pie -fno-pic -nostdlib -O0 -I./antem_libc -c robot.c -o robot.o
// i686-elf-ld -T app_linker.ld robot.o stdio.o stdlib.o string.o -o robot.bin
// cat header_gui.bin robot.bin > robot.edxi
// e2cp robot.edxi disk.img:/apps/robot.edxi

#include "antem_libc/stdio.h"
#include "antem_libc/stdlib.h"

#define AREA_DOMANDA  0
#define AREA_RISPOSTA 1

int main_win = 0;

static char domanda[1025];

// Lo stato viene letto una volta per fotogramma e riusato: cosi' il pulsante
// e il messaggio dicono sempre la stessa cosa.
static int stato = ROBOT_ASSENTE;

void cmd_invia(void) {
    if (stato != ROBOT_PRONTO) return;          // senza robottino non si parte

    gui_textarea_get(AREA_DOMANDA, domanda, sizeof(domanda));
    if (domanda[0] == '\0') return;

    gui_textarea_set(AREA_RISPOSTA, "");        // via la risposta precedente
    robot_ask(domanda, AREA_RISPOSTA);          // il kernel scrivera' lui nell'area
}

void cmd_pulisci(void) {
    gui_textarea_set(AREA_DOMANDA, "");
    gui_textarea_set(AREA_RISPOSTA, "");
}

void main() {
    gui_set_window_size(470, 430);
    gui_set_window_min_size(380, 330);

    gui_textarea_set(AREA_DOMANDA, "Hello! Who are you?");
    gui_textarea_set(AREA_RISPOSTA,
        "Response area:, "
        "The LLM response will appear here.");

    while (1) {
        gui_poll_events();
        stato = robot_status();

        gui_set_context(main_win);
        gui_clear();

        gui_text_bold(ANCHOR_TL, 10, 8, "Request", 0xFF000000);
        gui_textarea(ANCHOR_TL, 10, 28, 440, 90, 0xFFFFFFFF, AREA_DOMANDA, TEXTAREA_EDIT);

        gui_action_button(ANCHOR_TL, 10, 126, 100, 26, BUTTON_DEFAULT, 0xFF000000, "Send", cmd_invia);
        gui_action_button(ANCHOR_TL, 120, 126, 100, 26, BUTTON_DEFAULT, 0xFF000000, "Reset", cmd_pulisci);

        // Il messaggio di stato racconta sempre dov'e' il robottino
        if (stato == ROBOT_ASSENTE)
            gui_text_bold(ANCHOR_TL, 232, 132, "Please put the robot", 0xFFB03030);
        else if (stato == ROBOT_PENSA)
            gui_text_bold(ANCHOR_TL, 232, 132, "Thinking...", 0xFF2F5FD0);
        else
            gui_text_bold(ANCHOR_TL, 232, 132, "Robot ready", 0xFF2F7F3F);

        gui_text_bold(ANCHOR_TL, 10, 164, "Answer", 0xFF000000);
        gui_textarea(ANCHOR_TL, 10, 184, 440, 200, 0xFFF4F4F4, AREA_RISPOSTA, TEXTAREA_READONLY);

        gui_yield();
    }
}
