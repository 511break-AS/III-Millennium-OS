// Third-Millennium Operating System - Media Player
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

// COMPILAZIONE + CONVERSIONE + INSTALLAZIONE
// i686-elf-gcc -m32 -ffreestanding -fno-pie -fno-pic -nostdlib -O0 -c antem_libc/stdio.c -o stdio.o
// i686-elf-gcc -m32 -ffreestanding -fno-pie -fno-pic -nostdlib -O0 -c antem_libc/stdlib.c -o stdlib.o
// i686-elf-gcc -m32 -ffreestanding -fno-pie -fno-pic -nostdlib -O0 -c antem_libc/string.c -o string.o
// i686-elf-gcc -m32 -ffreestanding -fno-pie -fno-pic -nostdlib -O0 -I./antem_libc -c player.c -o player.o
// i686-elf-ld -T app_linker.ld player.o stdio.o stdlib.o string.o -o player.bin
// cat header_gui.bin player.bin > player.edxi
// e2cp player.edxi disk.img:/apps/player.edxi

#include "antem_libc/stdio.h"
#include "antem_libc/stdlib.h"

// Posizione del campo del percorso nell'ordine di disegno: gui_get_text lo
// ritrova cosi'. Se si aggiunge un elemento PRIMA del campo, va aggiornata.
#define IDX_PERCORSO 2

int main_win = 0;
int about_win = -1;
char audio_path[64] = "/system/media/startup.wav";
int is_playing = 0;

// ==========================================
// LE FUNZIONI (CALLBACKS) CHE REAGISCONO AI CLICK
// ==========================================
void cmd_play() {
    is_playing = 1;
    audio_play(audio_path);
}

void cmd_stop() {
    is_playing = 0;
    audio_stop();
}

void cmd_about() {
    if (about_win == -1) about_win = gui_spawn_window("Informazioni", 260, 130);
}

// ==========================================
// IL PROGRAMMA PRINCIPALE
// ==========================================
void main() {
    // Un lettore si sviluppa in larghezza: una riga per il file, una per i comandi
    gui_set_window_size(500, 170);
    gui_set_window_min_size(420, 170);

    int primo_frame = 1;

    while (1) {
        // 1. Legge globalmente mouse e tastiera
        gui_poll_events();
        (void)get_key();

        gui_set_context(main_win);

        // 2. Recupera il percorso che l'utente puo' aver modificato nel campo
        char temp_path[64];
        gui_get_text(IDX_PERCORSO, temp_path);

        if (primo_frame == 1) {
            primo_frame = 0;
        } else {
            int i = 0;
            while (temp_path[i] != '\0' && i < 63) { audio_path[i] = temp_path[i]; i++; }
            audio_path[i] = '\0';
        }

        if (about_win != -1 && gui_is_window_open(about_win) == 0) {
            about_win = -1;
        }

        // Il campo del percorso segue la larghezza della finestra. E' ancorato a
        // destra di proposito: il kernel non conta gli elementi ancorati a destra
        // nel calcolo della dimensione minima, altrimenti il minimo inseguirebbe
        // la finestra e non si potrebbe piu' stringerla.
        int ww = 0, wh = 0;
        get_window_size(&ww, &wh);
        int campo_w = ww - 96;
        if (campo_w < 120) campo_w = 120;

        // =======================================================
        // DISEGNO MAIN WINDOW
        // L'ordine conta: il campo del percorso deve restare il terzo (indice 2)
        // =======================================================
        gui_clear();

        gui_text_bold(ANCHOR_TL, 14, 12, "Lettore Audio AC'97", 0xFF000080);           // 0
        gui_text_bold(ANCHOR_TL, 14, 50, "File", 0xFF000000);                          // 1
        gui_textbox(ANCHOR_TR, 20, 46, campo_w, 24, 0xFFFFFFFF, 60, audio_path);       // 2

        // Verde scuro se in riproduzione, altrimenti verde acceso
        uint32_t play_color = is_playing ? 0xFF008000 : 0xFF00FF00;

        gui_action_button(ANCHOR_BL, 14, 12, 90, 30, play_color, 0xFF000000, "PLAY", cmd_play);
        gui_action_button(ANCHOR_BL, 112, 12, 90, 30, 0xFFFF0000, 0xFFFFFFFF, "STOP", cmd_stop);
        gui_action_button(ANCHOR_BR, 14, 12, 80, 30, BUTTON_DEFAULT, 0xFF000000, "About", cmd_about);

        // =======================================================
        // DISEGNO ABOUT WINDOW
        // =======================================================
        if (about_win != -1) {
            gui_set_context(about_win);
            gui_clear();
            gui_text_bold(ANCHOR_TC, 0, 24, "Ante-M Media Player v1.0", 0xFF000080);
            gui_text_bold(ANCHOR_TC, 0, 48, "Di Alberto Sanfelice", 0xFF000000);
        }

        gui_yield();
    }
}
