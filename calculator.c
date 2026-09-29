// Third-Millennium Operating System - Calculator
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
// i686-elf-gcc -m32 -ffreestanding -fno-pie -fno-pic -nostdlib -O0 -I./antem_libc -c calculator.c -o calculator.o
// i686-elf-ld -T app_linker.ld calculator.o stdio.o stdlib.o string.o -o calculator.bin
// cat header_gui.bin calculator.bin > calculator.edxi
// e2cp calculator.edxi disk.img:/apps/calculator.edxi
//
// Si usa anche da tastiera: cifre, punto (o virgola), + - * / e x,
// Invio o = per il risultato, Backspace per cancellare l'ultima cifra,
// C per azzerare, % per la percentuale.
//
// NOTA SULLA VIRGOLA MOBILE
// La calcolatrice lavora in double, cioe' con il coprocessore matematico x87.
// Per non dipendere da libgcc, che nel sistema non c'e', si usano solo le
// operazioni che il compilatore traduce in istruzioni dirette: somme,
// prodotti, divisioni e conversioni fra double e int a 32 bit. Il kernel non
// salva lo stato del coprocessore fra un processo e l'altro: finche' la
// calcolatrice e' l'unica app a usarlo non c'e' alcun problema.

#include "antem_libc/stdio.h"
#include "antem_libc/stdlib.h"

int main_win = 0;

// ==========================================
// ASPETTO
// ==========================================
#define VETRO        0xFF20404F   // tinta del display
#define CIFRE        0xFFE6F7FF   // colore delle cifre sul display
#define SEGNO_OP     0xFF9FD4E8   // operazione in sospeso, in piccolo
#define TASTO_GRIGIO 0xFFC9CCD2   // tutto cio' che non e' una cifra
#define TESTO        0xFF000000
// Le cifre usano il colore del tema (BUTTON_DEFAULT)

#define MAX_CIFRE    12           // cifre significative mostrate
#define LIMITE       1e12         // oltre, il display non basta: errore

// ==========================================
// STATO DELLA CALCOLATRICE
// ==========================================
static char   entry[24];          // il numero che l'utente sta digitando
static int    typing = 0;         // 1 = il display mostra entry
static int    operand_ready = 0;  // c'e' un nuovo operando dopo l'ultimo operatore
static double acc = 0;            // risultato accumulato
static double cur = 0;            // valore mostrato quando non si digita
static char   op = 0;             // operazione in sospeso: + - * /
static int    error = 0;
static char   shown[32];          // testo del display

// ==========================================
// NUMERI <-> TESTO
// ==========================================
static int slen(const char* s) { int n = 0; while (s[n]) n++; return n; }

static void scopy(char* d, const char* s) { while ((*d++ = *s++)) ; }

// Dal testo digitato al numero. Le cifre si accumulano come intero e si divide
// una volta sola alla fine: e' piu' preciso che sommare un decimo alla volta.
static double parse_entry(const char* s) {
    double v = 0.0, div = 1.0;
    int neg = 0, frac = 0;
    for (; *s; s++) {
        if (*s == '-') neg = 1;
        else if (*s == '.') frac = 1;
        else if (*s >= '0' && *s <= '9') {
            v = v * 10.0 + (double)(*s - '0');
            if (frac) div *= 10.0;
        }
    }
    v /= div;
    return neg ? -v : v;
}

// Dal numero al testo, con al massimo 12 cifre significative e senza zeri
// inutili in coda. Per restare in aritmetica a 32 bit il numero, gia'
// arrotondato e reso intero, viene spezzato in due meta' da 8 cifre.
static void format_number(double v, char* out) {
    if (v != v || v >= LIMITE || v <= -LIMITE) { scopy(out, "Errore"); return; }

    int neg = (v < 0);
    if (neg) v = -v;

    // Quante cifre ha la parte intera: le restanti vanno ai decimali
    int k = 1;
    double p = 10.0;
    while (k < MAX_CIFRE && v >= p) { k++; p *= 10.0; }
    // Sotto l'uno lo zero davanti non e' una cifra significativa: tutte e
    // dodici vanno ai decimali (1/3 = 0.333333333333)
    int dec = (v < 1.0) ? MAX_CIFRE : MAX_CIFRE - k;

    double scale = 1.0;
    for (int i = 0; i < dec; i++) scale *= 10.0;

    // Arrotondamento: +0.5 e poi troncamento
    double n = v * scale + 0.5;
    int hi = (int)(n / 1e8);
    double lo = n - (double)hi * 1e8;
    if (lo >= 1e8) { hi++; lo -= 1e8; }
    if (lo < 0)    { hi--; lo += 1e8; }
    int lo_i = (int)lo;

    // 15 cifre con gli zeri davanti: 7 dalla meta' alta, 8 dalla bassa
    char d[15];
    for (int i = 6; i >= 0; i--)  { d[i] = (char)('0' + hi % 10);   hi /= 10; }
    for (int i = 14; i >= 7; i--) { d[i] = (char)('0' + lo_i % 10); lo_i /= 10; }

    int point = 15 - dec;                       // dove cade la virgola
    int first = 0;
    while (first < point - 1 && d[first] == '0') first++;   // via gli zeri davanti

    int last = 14;
    while (last >= point && d[last] == '0') last--;         // via gli zeri in coda

    int all_zero = 1;
    for (int i = first; i <= last; i++) if (d[i] != '0') { all_zero = 0; break; }

    int o = 0;
    if (neg && !all_zero) out[o++] = '-';       // niente "-0"
    for (int i = first; i < point; i++) out[o++] = d[i];
    if (last >= point) {
        out[o++] = '.';
        for (int i = point; i <= last; i++) out[o++] = d[i];
    }
    out[o] = '\0';
}

// ==========================================
// LOGICA
// ==========================================
static double value_now(void) { return typing ? parse_entry(entry) : cur; }

static void check_range(double v) {
    if (v != v || v >= LIMITE || v <= -LIMITE) error = 1;
}

static double compute(double a, char o, double b) {
    if (o == '+') return a + b;
    if (o == '-') return a - b;
    if (o == '*') return a * b;
    if (o == '/') {
        if (b == 0.0) { error = 1; return 0.0; }
        return a / b;
    }
    return b;
}

static void press_clear(void) {
    entry[0] = '\0';
    typing = 0; operand_ready = 0;
    acc = 0; cur = 0; op = 0; error = 0;
}

static void press_digit(char c) {
    if (error) return;
    if (!typing) { entry[0] = '\0'; typing = 1; }

    int n = slen(entry), digits = 0;
    for (int i = 0; i < n; i++) if (entry[i] >= '0' && entry[i] <= '9') digits++;
    // Lo zero da solo prima della virgola non e' una cifra significativa
    const char* e = (entry[0] == '-') ? entry + 1 : entry;
    if (e[0] == '0' && e[1] == '.') digits--;
    if (digits >= MAX_CIFRE) return;

    // Uno zero da solo davanti non ha senso: "0" seguito da "5" diventa "5"
    if ((n == 1 && entry[0] == '0') || (n == 2 && entry[0] == '-' && entry[1] == '0')) n--;

    entry[n] = c;
    entry[n + 1] = '\0';
    operand_ready = 1;
}

static void press_dot(void) {
    if (error) return;
    if (!typing) { scopy(entry, "0"); typing = 1; }
    for (int i = 0; entry[i]; i++) if (entry[i] == '.') return;   // uno solo
    int n = slen(entry);
    if (n == 0 || (n == 1 && entry[0] == '-')) entry[n++] = '0';
    entry[n] = '.';
    entry[n + 1] = '\0';
    operand_ready = 1;
}

static void press_back(void) {
    if (error || !typing) return;
    int n = slen(entry);
    if (n > 0) entry[--n] = '\0';
    if (n == 0 || (n == 1 && entry[0] == '-')) scopy(entry, "0");
}

static void press_op(char o) {
    if (error) return;
    double v = value_now();
    if (op && operand_ready) acc = compute(acc, op, v);    // catena: 2 + 3 + ...
    else if (!op)            acc = v;
    // op gia' presente e nessun nuovo operando: l'utente cambia idea, si sostituisce
    check_range(acc);
    op = o;
    typing = 0; operand_ready = 0;
    cur = acc;
}

static void press_equals(void) {
    if (error) return;
    double v = value_now();
    if (op) {
        // "5 + =" usa lo stesso numero: fa 10, come le calcolatrici tascabili
        acc = compute(acc, op, operand_ready ? v : acc);
        op = 0;
    } else {
        acc = v;
    }
    check_range(acc);
    typing = 0; operand_ready = 0;
    cur = acc;
}

// Come sulle calcolatrici tascabili: con + e - la percentuale e' calcolata sul
// primo numero (50 + 10% = 55), con x e / e' il numero diviso cento
// (50 x 10% = 5), da sola e' semplicemente il numero diviso cento.
static void press_percent(void) {
    if (error) return;
    double v = value_now();
    if (op == '+' || op == '-') cur = acc * v / 100.0;
    else                        cur = v / 100.0;
    typing = 0; operand_ready = 1;
    if (!op) acc = cur;
}

static void press_sign(void) {
    if (error) return;
    if (typing) {
        // Si aggiunge o si toglie il meno davanti al numero che si sta scrivendo
        int n = slen(entry);
        if (entry[0] == '-') { for (int i = 0; i < n; i++) entry[i] = entry[i + 1]; }
        else if (n < 22)     { for (int i = n; i >= 0; i--) entry[i + 1] = entry[i]; entry[0] = '-'; }
        return;
    }
    cur = -cur;
    if (!op) acc = cur;
    else operand_ready = 1;
}

static void update_display(void) {
    if (error)       scopy(shown, "Errore");
    else if (typing) scopy(shown, entry[0] ? entry : "0");
    else             format_number(cur, shown);
}

// ==========================================
// PULSANTI (una funzione per tasto, come vuole gui_action_button)
// ==========================================
void k0(void) { press_digit('0'); }  void k1(void) { press_digit('1'); }
void k2(void) { press_digit('2'); }  void k3(void) { press_digit('3'); }
void k4(void) { press_digit('4'); }  void k5(void) { press_digit('5'); }
void k6(void) { press_digit('6'); }  void k7(void) { press_digit('7'); }
void k8(void) { press_digit('8'); }  void k9(void) { press_digit('9'); }
void k_dot(void)  { press_dot(); }
void k_add(void)  { press_op('+'); }
void k_sub(void)  { press_op('-'); }
void k_mul(void)  { press_op('*'); }
void k_div(void)  { press_op('/'); }
void k_eq(void)   { press_equals(); }
void k_c(void)    { press_clear(); }
void k_sign(void) { press_sign(); }
void k_pct(void)  { press_percent(); }

static void handle_key(char k) {
    if (k >= '0' && k <= '9') press_digit(k);
    else if (k == '.' || k == ',') press_dot();
    else if (k == '+') press_op('+');
    else if (k == '-') press_op('-');
    else if (k == '*' || k == 'x' || k == 'X') press_op('*');
    else if (k == '/') press_op('/');
    else if (k == '=' || k == '\n') press_equals();
    else if (k == '\b') press_back();
    else if (k == 'c' || k == 'C') press_clear();
    else if (k == '%') press_percent();
}

// ==========================================
// IL PROGRAMMA PRINCIPALE
// ==========================================
// Pulsanti da 40x30, appena piu' grandi dell'etichetta piu' larga ("+/-"),
// su 4 colonne con 8 pixel di spazio; margini di 16 tutto attorno.
// La finestra e' 224 x 312: le misure sono calcolate in modo che il
// ridimensionamento automatico sui contenuti non la allarghi di nascosto
// (16 + 184 + 24 = 224 in larghezza, 232 + 30 + 40 = 302 < 312 in altezza).
#define BW      40
#define BH      30
#define GAP     8
#define COL(i)  (16 + (i) * (BW + GAP))
#define ROW(j)  (80 + (j) * (BH + GAP))
#define GRID_W  (4 * BW + 3 * GAP)          // 184: il display e' largo uguale

// Due scorciatoie per leggere meglio la griglia qui sotto
#define CIFRA(c, r, lab, cb)  gui_action_button(ANCHOR_TL, COL(c), ROW(r), BW, BH, BUTTON_DEFAULT, TESTO, lab, cb)
#define ALTRO(c, r, lab, cb)  gui_action_button(ANCHOR_TL, COL(c), ROW(r), BW, BH, TASTO_GRIGIO,   TESTO, lab, cb)

void main() {
    // Il coprocessore matematico va messo in uno stato noto: eccezioni
    // mascherate, precisione piena, arrotondamento al piu' vicino
    __asm__ volatile ("fninit");

    gui_set_window_size(224, 312);
    gui_set_window_min_size(224, 312);

    press_clear();

    while (1) {
        gui_poll_events();

        char k = get_key();
        if (k) handle_key(k);

        update_display();

        gui_set_context(main_win);
        gui_clear();

        // Il display: se il numero e' troppo lungo per le cifre grandi, il
        // kernel passa da solo a quelle normali
        gui_display(ANCHOR_TL, 16, 16, GRID_W, 52, VETRO, CIFRE, shown);

        // L'operazione in sospeso, piccola, nell'angolo del display
        if (op && !error) {
            char s[2] = { op == '*' ? 'x' : op, '\0' };
            gui_text_bold(ANCHOR_TL, 26, 22, s, SEGNO_OP);
        }

        ALTRO(0, 0, "C",   k_c);   ALTRO(1, 0, "+/-", k_sign); ALTRO(2, 0, "%", k_pct); ALTRO(3, 0, "/", k_div);
        CIFRA(0, 1, "7",   k7);    CIFRA(1, 1, "8",   k8);     CIFRA(2, 1, "9", k9);    ALTRO(3, 1, "x", k_mul);
        CIFRA(0, 2, "4",   k4);    CIFRA(1, 2, "5",   k5);     CIFRA(2, 2, "6", k6);    ALTRO(3, 2, "-", k_sub);
        CIFRA(0, 3, "1",   k1);    CIFRA(1, 3, "2",   k2);     CIFRA(2, 3, "3", k3);    ALTRO(3, 3, "+", k_add);

        // Lo zero occupa due colonne, come sulle calcolatrici vere
        gui_action_button(ANCHOR_TL, COL(0), ROW(4), BW * 2 + GAP, BH, BUTTON_DEFAULT, TESTO, "0", k0);
        ALTRO(2, 4, ".", k_dot);
        ALTRO(3, 4, "=", k_eq);

        gui_yield();
    }
}
