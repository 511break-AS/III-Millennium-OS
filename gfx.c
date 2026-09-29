#include "gfx.h"

// ==========================================
// MOTORE TEMI (THEME ENGINE)
// ==========================================


// Inizializziamo il sistema con il tuo attuale tema di default: "Desert Sand"
theme_palette_t active_theme = {
    .desktop_bg         = 0xFFA9BCC6,
    .primary_color      = 0xFFE3C6AA,
    .secondary_color    = 0xFFCFB298,
    .taskbar_bg         = 0xFF6E6E6E,
    .taskbar_btn_active = 0xFFA68A70,
    .taskbar_btn_idle   = 0xFFCFB298,
    .window_bg          = 0xFFC0C0C0,
    .window_close_btn   = 0xFFDF7B62,
    .window_ctrl_btn    = 0xFFC0C0C0, 
    .titlebar_base      = 0xFFE3C6AA, 
    .titlebar_lines     = 0xFFCFB298, 
    .menu_hover_bg      = 0xFFCFB298, 
    .menu_hover_lines   = 0xFFE3C6AA, 
    .tooltip_bg         = 0xFFE6E1D7,
    .scrollbar_track     = 0xFFE1E1E1,
    .button_body         = 0xFF9FBDE7,
    .button_body_pressed = 0xFF8EA9CE,
    .button_veil         = 0xFFD8FAFF,
    .button_rim          = 0xFF354B83,
    .button_rays         = 0xFFFFFFFF,
    .taskbar_glow        = 0xFF2D9CFF
};



// ==========================================
// SCHEDA VIDEO BGA E GRAFICA
// ==========================================
uint32_t* fb = (uint32_t*)0xFD000000;
int fb_width = 1024;
int fb_height = 768;

#define PANEL_BAR_H    24   // altezza barra superiore (menu bar)
#define PANEL_SHADOW_H 8    // altezza ombra sfumata sotto la barra

// IL DOUBLE BUFFER (Tela nascosta in RAM)
uint32_t backbuffer[1024 * 768];

// --- DIRTY RECTANGLES ---
// Specchio di ciò che si trova già nella memoria video: serve a capire
// quali zone sono davvero cambiate e vanno riscritte.
#define TILE_W 64
#define TILE_H 16
uint32_t* shadow_buffer = 0;
int shadow_ready = 0;




// ==========================================
// FONT A MATRICE (8x8)
// ==========================================
const uint8_t font8x8_A_Z[26][8] = {
    {0x18,0x3C,0x66,0x7E,0x66,0x66,0x66,0x00}, // A
    {0x7C,0x66,0x66,0x7C,0x66,0x66,0x7C,0x00}, // B
    {0x3C,0x66,0x60,0x60,0x60,0x66,0x3C,0x00}, // C
    {0x78,0x6C,0x66,0x66,0x66,0x6C,0x78,0x00}, // D
    {0x7E,0x60,0x60,0x78,0x60,0x60,0x7E,0x00}, // E
    {0x7E,0x60,0x60,0x78,0x60,0x60,0x60,0x00}, // F
    {0x3C,0x66,0x60,0x6E,0x66,0x66,0x3E,0x00}, // G
    {0x66,0x66,0x66,0x7E,0x66,0x66,0x66,0x00}, // H
    {0x3C,0x18,0x18,0x18,0x18,0x18,0x3C,0x00}, // I
    {0x1E,0x0C,0x0C,0x0C,0x0C,0x6C,0x38,0x00}, // J
    {0x66,0x6C,0x78,0x70,0x78,0x6C,0x66,0x00}, // K
    {0x60,0x60,0x60,0x60,0x60,0x60,0x7E,0x00}, // L
    {0x63,0x77,0x7F,0x6B,0x63,0x63,0x63,0x00}, // M
    {0x66,0x76,0x7E,0x7E,0x6E,0x66,0x66,0x00}, // N
    {0x3C,0x66,0x66,0x66,0x66,0x66,0x3C,0x00}, // O
    {0x7C,0x66,0x66,0x7C,0x60,0x60,0x60,0x00}, // P
    {0x3C,0x66,0x66,0x66,0x6A,0x6C,0x36,0x00}, // Q
    {0x7C,0x66,0x66,0x7C,0x6C,0x66,0x66,0x00}, // R
    {0x3C,0x66,0x60,0x3C,0x06,0x66,0x3C,0x00}, // S
    {0x7E,0x18,0x18,0x18,0x18,0x18,0x18,0x00}, // T
    {0x66,0x66,0x66,0x66,0x66,0x66,0x3C,0x00}, // U
    {0x66,0x66,0x66,0x66,0x66,0x3C,0x18,0x00}, // V
    {0x63,0x63,0x63,0x6B,0x7F,0x77,0x63,0x00}, // W
    {0x66,0x66,0x3C,0x18,0x3C,0x66,0x66,0x00}, // X
    {0x66,0x66,0x66,0x3C,0x18,0x18,0x18,0x00}, // Y
    {0x7E,0x06,0x0C,0x18,0x30,0x60,0x7E,0x00}  // Z
};



// L'Array del Font Proporzionale NATIVO 16px 
// Font proporzionale: ASCII (0-127) piu' le lettere accentate della codifica
// Latin-1 (192-255), che e' quella usata dentro il sistema. Le voci oltre il
// 127 sono in fondo, indicate per numero.
prop_char_t font_prop[256] = {
    {0,{0}},{0,{0}},{0,{0}},{0,{0}},{0,{0}},{0,{0}},{0,{0}},{0,{0}},{0,{0}},{0,{0}},{0,{0}},{0,{0}},{0,{0}},{0,{0}},{0,{0}},{0,{0}},
    {0,{0}},{0,{0}},{0,{0}},{0,{0}},{0,{0}},{0,{0}},{0,{0}},{0,{0}},{0,{0}},{0,{0}},{0,{0}},{0,{0}},{0,{0}},{0,{0}},{0,{0}},{0,{0}},
    {4, {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0}}, // Spazio
    {2, {0,0,0,0,0,0x80,0x80,0x80,0x80,0x80,0x80,0x00,0x80,0,0,0}}, // ! 
    {4, {0,0,0,0,0,0xA0,0xA0,0xA0,0x00,0,0,0,0,0,0,0}}, // "
    {6, {0,0,0,0,0,0x28,0x28,0xFC,0x28,0x28,0xFC,0x28,0x28,0,0,0}}, // #
    {5, {0,0,0,0,0,0x20,0x78,0xA0,0xA0,0x70,0x28,0xF0,0x20,0,0,0}}, // $
    {7, {0,0,0,0,0,0x42,0xA4,0x48,0x10,0x24,0x4A,0x84,0,0,0,0}}, // % 
    {6, {0,0,0,0,0,0x40,0xA0,0xA0,0x40,0xA8,0x90,0x88,0x70,0,0,0}}, // &
    {2, {0,0,0,0,0,0x80,0x80,0x80,0,0,0,0,0,0,0,0}}, // '
    {3, {0,0,0,0,0,0x40,0x80,0x80,0x80,0x80,0x80,0x80,0x40,0,0,0}}, // (
    {3, {0,0,0,0,0,0x80,0x40,0x40,0x40,0x40,0x40,0x40,0x80,0,0,0}}, // )
    {5, {0,0,0,0,0,0,0x20,0xA8,0x70,0xA8,0x20,0,0,0,0,0}}, // *
    {5, {0,0,0,0,0,0,0,0x20,0x20,0xF8,0x20,0x20,0,0,0,0}}, // +
    {2, {0,0,0,0,0,0,0,0,0,0,0,0,0x40,0x40,0x80,0}}, // , 
    {4, {0,0,0,0,0,0,0,0,0xF0,0,0,0,0,0,0,0}}, // -
    {2, {0,0,0,0,0,0,0,0,0,0,0,0,0x40,0x40,0,0}}, // . 
    {4, {0,0,0,0,0,0x10,0x10,0x20,0x20,0x40,0x40,0x80,0x80,0,0,0}}, // /
    {5, {0,0,0,0,0,0x70,0x88,0x88,0x88,0x88,0x88,0x88,0x70,0,0,0}}, // 0
    {4, {0,0,0,0,0,0x40,0xC0,0x40,0x40,0x40,0x40,0x40,0xE0,0,0,0}}, // 1
    {5, {0,0,0,0,0,0x70,0x88,0x08,0x08,0x10,0x20,0x40,0xF8,0,0,0}}, // 2
    {5, {0,0,0,0,0,0x70,0x88,0x08,0x08,0x30,0x08,0x88,0x70,0,0,0}}, // 3
    {5, {0,0,0,0,0,0x10,0x30,0x50,0x90,0x90,0xF8,0x10,0x10,0,0,0}}, // 4
    {5, {0,0,0,0,0,0xF8,0x80,0x80,0xF0,0x08,0x08,0x88,0x70,0,0,0}}, // 5
    {5, {0,0,0,0,0,0x30,0x40,0x80,0x80,0xF0,0x88,0x88,0x70,0,0,0}}, // 6
    {5, {0,0,0,0,0,0xF8,0x08,0x08,0x10,0x20,0x20,0x40,0x40,0,0,0}}, // 7
    {5, {0,0,0,0,0,0x70,0x88,0x88,0x70,0x88,0x88,0x88,0x70,0,0,0}}, // 8 
    {5, {0,0,0,0,0,0x70,0x88,0x88,0x88,0x78,0x08,0x10,0x60,0,0,0}}, // 9
    {2, {0,0,0,0,0,0,0,0x40,0x40,0,0,0x40,0x40,0,0,0}}, // : 
    {2, {0,0,0,0,0,0,0,0x40,0x40,0,0,0x40,0x40,0x80,0,0}}, // ; 
    {4, {0,0,0,0,0,0,0x10,0x20,0x40,0x80,0x40,0x20,0x10,0,0,0}}, // <
    {5, {0,0,0,0,0,0,0,0xF8,0,0,0xF8,0,0,0,0,0}}, // =
    {4, {0,0,0,0,0,0,0x80,0x40,0x20,0x10,0x20,0x40,0x80,0,0,0}}, // >
    {5, {0,0,0,0,0,0x70,0x88,0x08,0x10,0x20,0x20,0x00,0x20,0,0,0}}, // ? 
    {7, {0,0,0,0,0,0x38,0x44,0x9A,0xAA,0xAA,0x9C,0x40,0x3C,0,0,0}}, // @ 
    {6, {0,0,0,0,0,0x20,0x50,0x88,0x88,0xF8,0x88,0x88,0x88,0,0,0}}, // A
    {6, {0,0,0,0,0,0xF0,0x88,0x88,0xF0,0x88,0x88,0x88,0xF0,0,0,0}}, // B
    {6, {0,0,0,0,0,0x70,0x88,0x80,0x80,0x80,0x80,0x88,0x70,0,0,0}}, // C
    {6, {0,0,0,0,0,0xF0,0x88,0x88,0x88,0x88,0x88,0x88,0xF0,0,0,0}}, // D
    {5, {0,0,0,0,0,0xF8,0x80,0x80,0xF0,0x80,0x80,0x80,0xF8,0,0,0}}, // E
    {5, {0,0,0,0,0,0xF8,0x80,0x80,0xF0,0x80,0x80,0x80,0x80,0,0,0}}, // F
    {6, {0,0,0,0,0,0x70,0x88,0x80,0x80,0x98,0x88,0x88,0x70,0,0,0}}, // G
    {6, {0,0,0,0,0,0x88,0x88,0x88,0xF8,0x88,0x88,0x88,0x88,0,0,0}}, // H
    {3, {0,0,0,0,0,0xE0,0x40,0x40,0x40,0x40,0x40,0x40,0xE0,0,0,0}}, // I
    {5, {0,0,0,0,0,0x38,0x10,0x10,0x10,0x10,0x10,0x90,0x60,0,0,0}}, // J
    {6, {0,0,0,0,0,0x88,0x90,0xA0,0xC0,0xA0,0x90,0x88,0x88,0,0,0}}, // K
    {5, {0,0,0,0,0,0x80,0x80,0x80,0x80,0x80,0x80,0x80,0xF8,0,0,0}}, // L
    {7, {0,0,0,0,0,0x82,0xC6,0xAA,0x92,0x82,0x82,0x82,0x82,0,0,0}}, // M
    {6, {0,0,0,0,0,0x88,0xC8,0xA8,0xA8,0x98,0x98,0x88,0x88,0,0,0}}, // N
    {6, {0,0,0,0,0,0x70,0x88,0x88,0x88,0x88,0x88,0x88,0x70,0,0,0}}, // O
    {6, {0,0,0,0,0,0xF0,0x88,0x88,0xF0,0x80,0x80,0x80,0x80,0,0,0}}, // P
    {6, {0,0,0,0,0,0x70,0x88,0x88,0x88,0x88,0xA8,0x90,0x68,0,0,0}}, // Q
    {6, {0,0,0,0,0,0xF0,0x88,0x88,0xF0,0xA0,0x90,0x88,0x88,0,0,0}}, // R
    {6, {0,0,0,0,0,0x70,0x88,0x80,0x70,0x08,0x08,0x88,0x70,0,0,0}}, // S
    {5, {0,0,0,0,0,0xF8,0x20,0x20,0x20,0x20,0x20,0x20,0x20,0,0,0}}, // T
    {6, {0,0,0,0,0,0x88,0x88,0x88,0x88,0x88,0x88,0x88,0x70,0,0,0}}, // U
    {6, {0,0,0,0,0,0x88,0x88,0x88,0x88,0x50,0x50,0x50,0x20,0,0,0}}, // V
    {7, {0,0,0,0,0,0x82,0x82,0x82,0x82,0x92,0xAA,0xC6,0x82,0,0,0}}, // W
    {6, {0,0,0,0,0,0x88,0x88,0x50,0x20,0x50,0x88,0x88,0x88,0,0,0}}, // X
    {6, {0,0,0,0,0,0x88,0x88,0x50,0x20,0x20,0x20,0x20,0x20,0,0,0}}, // Y
    {6, {0,0,0,0,0,0xF8,0x08,0x10,0x20,0x40,0x40,0x80,0xF8,0,0,0}}, // Z
    {3, {0,0,0,0,0,0xC0,0x80,0x80,0x80,0x80,0x80,0x80,0xC0,0,0,0}}, // [
    {4, {0,0,0,0,0,0x80,0x80,0x40,0x40,0x20,0x20,0x10,0x10,0,0,0}}, // Backslash 
    {3, {0,0,0,0,0,0xC0,0x40,0x40,0x40,0x40,0x40,0x40,0xC0,0,0,0}}, // ]
    {5, {0,0,0,0,0,0x20,0x50,0x88,0,0,0,0,0,0,0,0}}, // ^
    {5, {0,0,0,0,0,0,0,0,0,0,0,0,0,0xF8,0,0}}, // _
    {3, {0,0,0,0,0,0x80,0x40,0,0,0,0,0,0,0,0,0}}, // `
    // Minuscole (Iniziano più in basso, riga 7)
    {5, {0,0,0,0,0,0,0,0x70,0x08,0x78,0x88,0x88,0x78,0,0,0}}, // a
    {5, {0,0,0,0x80,0x80,0x80,0x80,0xB0,0xC8,0x88,0x88,0xC8,0xB0,0,0,0}}, // b
    {5, {0,0,0,0,0,0,0,0x70,0x88,0x80,0x80,0x88,0x70,0,0,0}}, // c
    {5, {0,0,0,0x08,0x08,0x08,0x08,0x68,0x98,0x88,0x88,0x98,0x68,0,0,0}}, // d
    {5, {0,0,0,0,0,0,0,0x70,0x88,0xF8,0x80,0x88,0x70,0,0,0}}, // e
    {4, {0,0,0,0x30,0x40,0x40,0xE0,0x40,0x40,0x40,0x40,0x40,0x40,0,0,0}}, // f
    {5, {0,0,0,0,0,0,0,0x78,0x88,0x88,0x88,0x78,0x08,0x88,0x70,0}}, // g 
    {5, {0,0,0,0x80,0x80,0x80,0x80,0xB0,0xC8,0x88,0x88,0x88,0x88,0,0,0}}, // h
    {2, {0,0,0,0x80,0x80,0,0,0x80,0x80,0x80,0x80,0x80,0x80,0,0,0}}, // i
    {3, {0,0,0,0x40,0x40,0,0,0x40,0x40,0x40,0x40,0x40,0x40,0x40,0x80,0}}, // j 
    {5, {0,0,0,0x80,0x80,0x80,0x80,0x88,0x90,0xE0,0x90,0x88,0x88,0,0,0}}, // k
    {2, {0,0,0,0x80,0x80,0x80,0x80,0x80,0x80,0x80,0x80,0x80,0x80,0,0,0}}, // l
    {7, {0,0,0,0,0,0,0,0xEC,0x92,0x92,0x92,0x92,0x92,0,0,0}}, // m
    {5, {0,0,0,0,0,0,0,0xB0,0xC8,0x88,0x88,0x88,0x88,0,0,0}}, // n
    {5, {0,0,0,0,0,0,0,0x70,0x88,0x88,0x88,0x88,0x70,0,0,0}}, // o
    {5, {0,0,0,0,0,0,0,0xB0,0xC8,0x88,0x88,0xC8,0xB0,0x80,0x80,0}}, // p 
    {5, {0,0,0,0,0,0,0,0x68,0x98,0x88,0x88,0x98,0x68,0x08,0x08,0}}, // q 
    {4, {0,0,0,0,0,0,0,0xB0,0xC0,0x80,0x80,0x80,0x80,0,0,0}}, // r
    {5, {0,0,0,0,0,0,0,0x78,0x80,0x80,0x70,0x08,0xF0,0,0,0}}, // s
    {4, {0,0,0,0x40,0x40,0x40,0xE0,0x40,0x40,0x40,0x40,0x40,0x30,0,0,0}}, // t
    {5, {0,0,0,0,0,0,0,0x88,0x88,0x88,0x88,0x98,0x68,0,0,0}}, // u
    {5, {0,0,0,0,0,0,0,0x88,0x88,0x88,0x50,0x50,0x20,0,0,0}}, // v
    {7, {0,0,0,0,0,0,0,0x82,0x92,0x92,0x92,0xAA,0x44,0,0,0}}, // w
    {5, {0,0,0,0,0,0,0,0x88,0x88,0x50,0x20,0x50,0x88,0,0,0}}, // x
    {5, {0,0,0,0,0,0,0,0x88,0x88,0x88,0x88,0x78,0x08,0x88,0x70,0}}, // y 
    {5, {0,0,0,0,0,0,0,0xF8,0x10,0x20,0x40,0x80,0xF8,0,0,0}}, // z
    {4, {0,0,0,0,0,0x30,0x40,0x40,0x40,0x80,0x40,0x40,0x40,0x30,0,0}}, // {
    {2, {0,0,0,0,0,0x80,0x80,0x80,0x80,0x80,0x80,0x80,0x80,0x80,0,0}}, // |
    {4, {0,0,0,0,0,0xC0,0x20,0x20,0x20,0x10,0x20,0x20,0x20,0xC0,0,0}}, // }
    {6, {0,0,0,0,0,0,0,0,0x48,0xB4,0,0,0,0,0,0}}, // ~
    {0, {0}},

    // --- Lettere accentate (Latin-1), ricavate dalle vocali qui sopra ---
    [0xC0] = {6, {0,0,0x40,0x20,0,0x20,0x50,0x88,0x88,0xF8,0x88,0x88,0x88,0,0,0}}, // A grave
    [0xC8] = {5, {0,0,0x40,0x20,0,0xF8,0x80,0x80,0xF0,0x80,0x80,0x80,0xF8,0,0,0}}, // E grave
    [0xC9] = {5, {0,0,0x20,0x40,0,0xF8,0x80,0x80,0xF0,0x80,0x80,0x80,0xF8,0,0,0}}, // E acuta
    [0xCC] = {3, {0,0,0x80,0x40,0,0xE0,0x40,0x40,0x40,0x40,0x40,0x40,0xE0,0,0,0}}, // I grave
    [0xD2] = {6, {0,0,0x40,0x20,0,0x70,0x88,0x88,0x88,0x88,0x88,0x88,0x70,0,0,0}}, // O grave
    [0xD9] = {6, {0,0,0x40,0x20,0,0x88,0x88,0x88,0x88,0x88,0x88,0x88,0x70,0,0,0}}, // U grave
    [0xE0] = {5, {0,0,0,0,0x40,0x20,0,0x70,0x08,0x78,0x88,0x88,0x78,0,0,0}}, // a grave
    [0xE1] = {5, {0,0,0,0,0x20,0x40,0,0x70,0x08,0x78,0x88,0x88,0x78,0,0,0}}, // a acuta
    [0xE8] = {5, {0,0,0,0,0x40,0x20,0,0x70,0x88,0xF8,0x80,0x88,0x70,0,0,0}}, // e grave
    [0xE9] = {5, {0,0,0,0,0x20,0x40,0,0x70,0x88,0xF8,0x80,0x88,0x70,0,0,0}}, // e acuta
    [0xEC] = {2, {0,0,0,0,0x80,0x40,0,0x80,0x80,0x80,0x80,0x80,0x80,0,0,0}}, // i grave
    [0xED] = {2, {0,0,0,0,0x40,0x80,0,0x80,0x80,0x80,0x80,0x80,0x80,0,0,0}}, // i acuta
    [0xF2] = {5, {0,0,0,0,0x40,0x20,0,0x70,0x88,0x88,0x88,0x88,0x70,0,0,0}}, // o grave
    [0xF3] = {5, {0,0,0,0,0x20,0x40,0,0x70,0x88,0x88,0x88,0x88,0x70,0,0,0}}, // o acuta
    [0xF9] = {5, {0,0,0,0,0x40,0x20,0,0x88,0x88,0x88,0x88,0x98,0x68,0,0,0}}, // u grave
    [0xFA] = {5, {0,0,0,0,0x20,0x40,0,0x88,0x88,0x88,0x88,0x98,0x68,0,0,0}}, // u acuta
};


// ==========================================
// FONT MINUSCOLO "HIGH-LEGIBILITY" (a-z)
// ==========================================
const uint8_t font8x8_a_z[26][8] = {
    {0x00,0x00,0x3C,0x06,0x3E,0x66,0x3B,0x00}, // a
    {0x60,0x60,0x7C,0x66,0x66,0x66,0x7C,0x00}, // b
    {0x00,0x00,0x3C,0x60,0x60,0x60,0x3C,0x00}, // c
    {0x06,0x06,0x3E,0x66,0x66,0x66,0x3E,0x00}, // d
    {0x00,0x00,0x3C,0x66,0x7E,0x60,0x3C,0x00}, // e
    {0x1C,0x36,0x30,0x78,0x30,0x30,0x30,0x00}, // f
    {0x00,0x00,0x3B,0x66,0x66,0x3E,0x06,0x3C}, // g 
    {0x60,0x60,0x7C,0x66,0x66,0x66,0x66,0x00}, // h
    {0x18,0x18,0x00,0x38,0x18,0x18,0x3C,0x00}, // i 
    {0x0C,0x0C,0x00,0x0C,0x0C,0x0C,0xCC,0x78}, // j 
    {0x60,0x60,0x66,0x6C,0x78,0x6C,0x66,0x00}, // k
    {0x38,0x18,0x18,0x18,0x18,0x18,0x3C,0x00}, // l
    {0x00,0x00,0xEE,0xDB,0xDB,0xDB,0xDB,0x00}, // m 
    {0x00,0x00,0x7C,0x66,0x66,0x66,0x66,0x00}, // n
    {0x00,0x00,0x3C,0x66,0x66,0x66,0x3C,0x00}, // o
    {0x00,0x00,0x7C,0x66,0x66,0x7C,0x60,0x60}, // p
    {0x00,0x00,0x3E,0x66,0x66,0x3E,0x06,0x06}, // q
    {0x00,0x00,0x7C,0x66,0x60,0x60,0x60,0x00}, // r
    {0x00,0x00,0x3E,0x60,0x3C,0x06,0x7C,0x00}, // s
    {0x30,0x30,0x78,0x30,0x30,0x30,0x1C,0x00}, // t
    {0x00,0x00,0x66,0x66,0x66,0x66,0x3E,0x00}, // u
    {0x00,0x00,0x66,0x66,0x66,0x3C,0x18,0x00}, // v
    {0x00,0x00,0xDB,0xDB,0xDB,0xDB,0x77,0x00}, // w 
    {0x00,0x00,0x66,0x3C,0x18,0x3C,0x66,0x00}, // x
    {0x00,0x00,0x66,0x66,0x66,0x3E,0x06,0x3C}, // y
    {0x00,0x00,0x7E,0x0C,0x18,0x30,0x7E,0x00}  // z
};


const uint8_t font8x8_numbers[10][8] = {
    {0x3C,0x66,0x66,0x66,0x66,0x66,0x3C,0x00}, // 0
    {0x18,0x38,0x18,0x18,0x18,0x18,0x3E,0x00}, // 1
    {0x3C,0x66,0x06,0x1C,0x30,0x60,0x7E,0x00}, // 2
    {0x3C,0x66,0x06,0x1C,0x06,0x66,0x3C,0x00}, // 3
    {0x0C,0x1C,0x2C,0x4C,0x7E,0x0C,0x0C,0x00}, // 4
    {0x7E,0x60,0x7C,0x06,0x06,0x66,0x3C,0x00}, // 5
    {0x3C,0x60,0x7C,0x66,0x66,0x66,0x3C,0x00}, // 6
    {0x7E,0x06,0x06,0x0C,0x18,0x18,0x18,0x00}, // 7
    {0x3C,0x66,0x66,0x3C,0x66,0x66,0x3C,0x00}, // 8
    {0x3C,0x66,0x66,0x3E,0x06,0x0C,0x38,0x00}  // 9
};

const uint8_t font_space[8] = {0,0,0,0,0,0,0,0};





void draw_pixel(int x, int y, uint32_t color) {
    if (x >= 0 && x < fb_width && y >= 0 && y < fb_height) {
        // Scriviamo nella RAM, non sulla scheda video
        backbuffer[y * fb_width + x] = color;
    }
}


// ============================================================================
// CURSORE HARDWARE-STYLE (save-under)
// Il cursore NON viene mai disegnato nel backbuffer: il backbuffer resta la
// scena "pulita". Ripristinare lo sfondo = ricopiare quei pixel dal backbuffer.
// ============================================================================
#define CURSOR_W 12
#define CURSOR_H 16

static const uint8_t s90s_cursor[16][12] = {
    {1,0,0,0,0,0,0,0,0,0,0,0},
    {1,1,0,0,0,0,0,0,0,0,0,0},
    {1,2,1,0,0,0,0,0,0,0,0,0},
    {1,2,2,1,0,0,0,0,0,0,0,0},
    {1,2,2,2,1,0,0,0,0,0,0,0},
    {1,2,2,2,2,1,0,0,0,0,0,0},
    {1,2,2,2,2,2,1,0,0,0,0,0},
    {1,2,2,2,2,2,2,1,0,0,0,0},
    {1,2,2,2,2,2,2,2,1,0,0,0},
    {1,2,2,2,2,2,2,2,2,1,0,0},
    {1,2,2,2,2,2,1,1,1,1,0,0},
    {1,2,2,1,2,2,1,0,0,0,0,0},
    {1,2,1,0,1,2,2,1,0,0,0,0},
    {1,1,0,0,1,2,2,1,0,0,0,0},
    {1,0,0,0,0,1,2,2,1,0,0,0},
    {0,0,0,0,0,0,1,1,0,0,0,0}
};

int cursor_drawn_x = -1;   // dove si trova ORA la freccia disegnata su schermo
int cursor_drawn_y = -1;

void swap_buffers() {
    // Heap non ancora pronto, o primo frame: riversiamo tutto e inizializziamo lo specchio.
    if (!shadow_buffer || !shadow_ready) {
        int total = fb_width * fb_height;
        for (int i = 0; i < total; i++) fb[i] = backbuffer[i];
        if (shadow_buffer) {
            for (int i = 0; i < total; i++) shadow_buffer[i] = backbuffer[i];
            shadow_ready = 1;
        }
        return;
    }

    // Rettangolo della freccia: i suoi pixel pieni non vanno mai sovrascritti
    int has_cur = (cursor_drawn_x >= 0);
    int cx0 = 0, cy0 = 0, cx1 = 0, cy1 = 0;
    if (has_cur) {
        cx0 = cursor_drawn_x;  cy0 = cursor_drawn_y;
        cx1 = cx0 + CURSOR_W;  cy1 = cy0 + CURSOR_H;
    }

    for (int ty = 0; ty < fb_height; ty += TILE_H) {
        int y_end = ty + TILE_H; if (y_end > fb_height) y_end = fb_height;

        for (int tx = 0; tx < fb_width; tx += TILE_W) {
            int x_end = tx + TILE_W; if (x_end > fb_width) x_end = fb_width;

            // 1. Questa tessera è cambiata? Confronto in RAM: costa pochissimo.
            int dirty = 0;
            for (int y = ty; y < y_end && !dirty; y++) {
                int base = y * fb_width;
                for (int x = tx; x < x_end; x++) {
                    if (backbuffer[base + x] != shadow_buffer[base + x]) { dirty = 1; break; }
                }
            }
            if (!dirty) continue;   // invariata: la memoria video non viene toccata

            // 2. Solo le tessere cambiate raggiungono la VRAM (la parte lenta)
            for (int y = ty; y < y_end; y++) {
                int base = y * fb_width;
                int on_cur = (has_cur && y >= cy0 && y < cy1);
                int ry = on_cur ? (y - cy0) : 0;

                for (int x = tx; x < x_end; x++) {
                    uint32_t c = backbuffer[base + x];
                    shadow_buffer[base + x] = c;

                    // I pixel pieni della freccia restano intatti: niente sfarfallio
                    if (on_cur && x >= cx0 && x < cx1 && s90s_cursor[ry][x - cx0] != 0) continue;

                    fb[base + x] = c;
                }
            }
        }
    }
}




// Cancella la freccia ricopiando la scena pulita dal backbuffer
void cursor_restore() {
    if (cursor_drawn_x < 0) return;

    int x0 = cursor_drawn_x, y0 = cursor_drawn_y;
    int x1 = x0 + CURSOR_W,  y1 = y0 + CURSOR_H;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > fb_width)  x1 = fb_width;
    if (y1 > fb_height) y1 = fb_height;

    for (int y = y0; y < y1; y++) {
        int base = y * fb_width;
        for (int x = x0; x < x1; x++) fb[base + x] = backbuffer[base + x];
    }
    cursor_drawn_x = -1;
    cursor_drawn_y = -1;
}

// Disegna la freccia DIRETTAMENTE sullo schermo (non nel backbuffer)
void cursor_draw_at(int mx, int my) {
    for (int cy = 0; cy < CURSOR_H; cy++) {
        int py = my + cy;
        if (py < 0 || py >= fb_height) continue;
        int base = py * fb_width;
        for (int cx = 0; cx < CURSOR_W; cx++) {
            int px = mx + cx;
            if (px < 0 || px >= fb_width) continue;
            uint8_t v = s90s_cursor[cy][cx];
            if (v == 1)      fb[base + px] = 0xFF000000;  // bordo nero
            else if (v == 2) fb[base + px] = 0xFFFFFFFF;  // riempimento
        }
    }
    cursor_drawn_x = mx;
    cursor_drawn_y = my;
}


// Cancella la freccia dalla VECCHIA posizione senza toccare i pixel
// occupati dalla freccia NUOVA (che è già stata disegnata).
void cursor_erase_old(int ox, int oy, int nx, int ny) {
    int x0 = ox, y0 = oy;
    int x1 = ox + CURSOR_W, y1 = oy + CURSOR_H;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > fb_width)  x1 = fb_width;
    if (y1 > fb_height) y1 = fb_height;

    for (int y = y0; y < y1; y++) {
        int base = y * fb_width;
        int ry = y - ny;
        for (int x = x0; x < x1; x++) {
            int rx = x - nx;
            // Se questo pixel appartiene alla freccia NUOVA, non lo tocchiamo
            if (rx >= 0 && rx < CURSOR_W && ry >= 0 && ry < CURSOR_H &&
                s90s_cursor[ry][rx] != 0) continue;
            fb[base + x] = backbuffer[base + x];
        }
    }
}


// Spostamento SENZA sfarfallio: prima appare la freccia nuova,
// poi si ripulisce la vecchia. Il cursore non è mai assente dallo schermo.
void cursor_move(int mx, int my) {
    int ox = cursor_drawn_x;
    int oy = cursor_drawn_y;

    cursor_draw_at(mx, my);                    // 1. la freccia nuova c'è già
    if (ox >= 0) cursor_erase_old(ox, oy, mx, my);  // 2. via la scia vecchia
}



void draw_rect(int start_x, int start_y, int width, int height, uint32_t color) {
    // 1. Tagliamo i bordi fuori dallo schermo UNA SOLA VOLTA (Clipping)
    int x0 = start_x; if (x0 < 0) x0 = 0;
    int y0 = start_y; if (y0 < 0) y0 = 0;
    int x1 = start_x + width; if (x1 > fb_width) x1 = fb_width;
    int y1 = start_y + height; if (y1 > fb_height) y1 = fb_height;

    // 2. Scrittura Diretta in RAM 
    for (int y = y0; y < y1; y++) {
        // Calcoliamo l'indirizzo di partenza della riga
        uint32_t* row_ptr = &backbuffer[y * fb_width + x0];
        
        for (int x = x0; x < x1; x++) {
            *row_ptr = color; // Coloriamo il pixel
            row_ptr++;        // Passiamo al prossimo (operazione istantanea per la CPU)
        }
    }
}


// ============================================================================
// ANTIALIASING: fondamenta
// ============================================================================

// Miscela un colore sul backbuffer con una copertura 0..255
void blend_pixel(int x, int y, uint32_t color, int alpha) {
    if (alpha <= 0) return;
    if (x < 0 || x >= fb_width || y < 0 || y >= fb_height) return;
    if (y >= gfx_clip_y1) return;

    uint32_t* p = &backbuffer[y * fb_width + x];
    if (alpha >= 255) { *p = color; return; }

    uint32_t sr = (color >> 16) & 0xFF, sg = (color >> 8) & 0xFF, sb = color & 0xFF;
    uint32_t d  = *p;
    uint32_t dr = (d >> 16) & 0xFF,     dg = (d >> 8) & 0xFF,     db = d & 0xFF;
    int ia = 255 - alpha;

    uint32_t r = (sr * alpha + dr * ia) / 255;
    uint32_t g = (sg * alpha + dg * ia) / 255;
    uint32_t b = (sb * alpha + db * ia) / 255;
    *p = 0xFF000000 | (r << 16) | (g << 8) | b;
}

// Quanta parte del pixel (px,py) cade dentro il rettangolo arrotondato?
// Ritorna 0..255. Nessuna radice quadrata: solo distanze al quadrato.
int rrect_coverage(int px, int py, int x, int y, int w, int h, int rad) {
    if (px < x || px >= x + w || py < y || py >= y + h) return 0;

    // Zona sicura: lontano dagli angoli il pixel e' pieno, niente calcoli
    if ((px >= x + rad && px < x + w - rad) ||
        (py >= y + rad && py < y + h - rad)) return 255;

    // Vicino a un angolo: 16 sotto-campioni, in ottavi di pixel
    int L = x * 8, R = (x + w) * 8, T = y * 8, B = (y + h) * 8;
    int r8 = rad * 8;
    int hits = 0;

    for (int sy = 0; sy < 4; sy++) {
        int fy = py * 8 + sy * 2 + 1;
        for (int sx = 0; sx < 4; sx++) {
            int fx = px * 8 + sx * 2 + 1;
            int cx, cy;

            if      (fx < L + r8) cx = L + r8;
            else if (fx > R - r8) cx = R - r8;
            else { hits++; continue; }          // fascia verticale: dentro

            if      (fy < T + r8) cy = T + r8;
            else if (fy > B - r8) cy = B - r8;
            else { hits++; continue; }          // fascia orizzontale: dentro

            int dx = fx - cx, dy = fy - cy;
            if (dx * dx + dy * dy <= r8 * r8) hits++;
        }
    }
    return hits * 255 / 16;
}

// Curva a S: parte e arriva con pendenza nulla, quindi la sfumatura
// si fonde nella zona piena senza lasciare uno spigolo visibile.
int smooth255(int s) {
    if (s <= 0)   return 0;
    if (s >= 255) return 255;
    int a = s * s / 255;
    return 3 * a - 2 * a * s / 255;
}

int circle_coverage(int px, int py, int cx, int cy, int rad) {
    int dx = px - cx, dy = py - cy;
    int outer = rad + 1, inner = rad - 1;
    int d2 = dx * dx + dy * dy;
    if (d2 >= outer * outer) return 0;
    if (d2 <= inner * inner) return 255;

    int c8x = cx * 8 + 4, c8y = cy * 8 + 4, r8 = rad * 8;
    int hits = 0;
    for (int sy = 0; sy < 4; sy++) {
        int fy = py * 8 + sy * 2 + 1 - c8y;
        for (int sx = 0; sx < 4; sx++) {
            int fx = px * 8 + sx * 2 + 1 - c8x;
            if (fx * fx + fy * fy <= r8 * r8) hits++;
        }
    }
    return hits * 255 / 16;
}

void fill_circle_aa(int cx, int cy, int rad, uint32_t color) {
    for (int py = cy - rad - 1; py <= cy + rad + 1; py++)
        for (int px = cx - rad - 1; px <= cx + rad + 1; px++)
            blend_pixel(px, py, color, circle_coverage(px, py, cx, cy, rad));
}

// Ombra proiettata sotto un bottone. Va chiamata PRIMA di disegnarlo,
// perche' scurisce i pixel gia' presenti nel backbuffer.
void draw_button_shadow(int x, int y, int width, int height, int rad, int pressed) {
    (void)pressed;                  // ombra identica in entrambi gli stati

    int SPREAD = 4;   // numero di passi: piu' alto = sfumatura piu' graduale
    int DY     = 1;   // quanto scende
    int DARK   = 14;  // intensita' massima, subito sotto il bordo (% di nero)

    int x0 = x - SPREAD,          y0 = y - SPREAD + DY;
    int x1 = x + width + SPREAD,  y1 = y + height + SPREAD + DY;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > fb_width)  x1 = fb_width;
    if (y1 > fb_height) y1 = fb_height;

    for (int py = y0; py < y1; py++) {
        for (int px = x0; px < x1; px++) {
            // Niente ombra sotto la sagoma: il vetro e' traslucido e la lascerebbe passare
            if (rrect_coverage(px, py, x, y, width, height, rad) >= 250) continue;

            // Somma di sagome crescenti = sfumatura graduale
            int acc = 0;
            for (int k = 1; k <= SPREAD; k++)
                acc += rrect_coverage(px, py, x - k, y - k + DY,
                                      width + 2 * k, height + 2 * k, rad + k);
            int s = acc / SPREAD;
            if (s <= 0) continue;
            s = s * s / 255;               // caduta quadratica: si dirada allontanandosi
            if (s < 2) continue;           // taglia la coda invisibile (risparmia lavoro)

            int a = DARK * s / 255;
            uint32_t* p = &backbuffer[py * fb_width + px];
            uint32_t d = *p;
            uint32_t r = ((d >> 16) & 0xFF) * (100 - a) / 100;
            uint32_t g = ((d >> 8)  & 0xFF) * (100 - a) / 100;
            uint32_t b = (d & 0xFF)         * (100 - a) / 100;
            *p = 0xFF000000 | (r << 16) | (g << 8) | b;
        }
    }
}

// Manopole di draw_button(). I default riproducono il comportamento storico.
int      gfx_veil_bot       = 98;          // fine del velo, % altezza
int      gfx_veil_feather   = 32;          // sfumatura in uscita del velo, %
int      gfx_button_opacity = 210;         // 255 = opaco, 210 = storico
int      gfx_button_radius  = -1;          // -1 = automatico (capsula)
uint32_t gfx_veil_tint      = 0xFFFFFFFF;  // tinta verso cui sfuma il velo
uint32_t gfx_rim_tint       = 0;           // 0 = bordo ricavato dal corpo
// Riga oltre la quale non si disegna. Serve a mostrare solo una parte di una
// figura composta, come la mascotte seduta nel vassoio della barra superiore.
// Va sempre riportata al valore di riposo dopo l'uso.
int gfx_clip_y1 = 1 << 30;

void draw_button(int x, int y, int width, int height, uint32_t bg_color, uint32_t light_color, uint32_t dark_color) {
    (void)dark_color;

    // Il chiamante segnala la pressione passando un light_color diverso dal bianco
    int pressed = (light_color != 0xFFFFFFFF);

    const int RAD_MAX = 10;   // capsula Aqua; abbassalo per angoli meno tondi
    if (height < 6 || width < 6) return;

    int rad;
    if (gfx_button_radius >= 0) {
        rad = gfx_button_radius;             // forzato dal chiamante
    } else {
        rad = height / 2;                    // capsula Aqua
        if (rad > RAD_MAX) rad = RAD_MAX;
    }
    if (rad > width / 2) rad = width / 2;
    if (rad < 2) rad = 2;
    // L'ombra va stesa PRIMA del vetro
    draw_button_shadow(x, y, width, height, rad, pressed);

    int br = (bg_color >> 16) & 0xFF;
    int bg = (bg_color >> 8) & 0xFF;
    int bb = bg_color & 0xFF;

    // Il chiamante ha gia' scurito del 25% per la pressione: lo ANNULLIAMO.
    // Il "premuto" lo costruiamo qui, senza spegnere il vetro.
    if (pressed) {
        br = br * 4 / 3; if (br > 255) br = 255;
        bg = bg * 4 / 3; if (bg > 255) bg = 255;
        bb = bb * 4 / 3; if (bb > 255) bb = 255;
    }

    // --- MANOPOLE (riposo / premuto) ---
    int GLARE     = pressed ? 195 : 240;   // forza del riflesso
    int GLARE_TOP = pressed ?  60 :  75;   // plateau a piena luce
    int GLARE_PCT = pressed ?  24 :  30;   // il riflesso occupa solo il tratto alto
    int WAIST     = pressed ?  24 :  27;   // la vita: subito sotto il riflesso
    int PEAK      = pressed ?  70 :  72;   // dove il vetro e' PIU' luminoso
    int DIP       = pressed ?  16 :  11;   // profondita' della vita
    int LIFT      = pressed ?  44 :  50;   // luminosita' al picco
    int ENDL      = pressed ?  18 :  22;   // luminosita' sul bordo basso
    int OPACITY   = gfx_button_opacity;    // 255 = opaco; storico del kernel: 210
    int VEIL      = pressed ? 165 : 145;  // intensita' della velatura
    int VEIL_TOP  = 52;                   // dove inizia (% altezza) - piu' in basso
    int VEIL_BOT  = gfx_veil_bot;         // dove finisce (% altezza)
    int VEIL_FEAT = 48;                   // sfumatura in ALTO (%)
    int VEIL_FEB  = gfx_veil_feather;     // sfumatura in BASSO (%)

    // Il bordo Aqua non e' il corpo scurito: e' scurito E piu' saturo, e nessuna
    // percentuale uniforme ci arriva. Per questo esiste gfx_rim_tint.
    uint32_t rim;
    if (gfx_rim_tint)
        rim = gfx_rim_tint;
    else
        rim = 0xFF000000
            | ((br * 64 / 100) << 16) | ((bg * 64 / 100) << 8) | (bb * 64 / 100);

    // Il bagliore basso di Aqua e' un ciano (#D5F5FD), non un bianco: con una
    // velatura bianca quel colore non e' raggiungibile per nessun bg.
    int tr = (gfx_veil_tint >> 16) & 0xFF;
    int tg = (gfx_veil_tint >>  8) & 0xFF;
    int tb =  gfx_veil_tint        & 0xFF;
    uint32_t bloom_col = 0xFF000000
        | ((br + (tr - br) * 94 / 100) << 16)
        | ((bg + (tg - bg) * 94 / 100) << 8)
        |  (bb + (tb - bb) * 94 / 100);

    int gl_x = x + 2, gl_w = width - 4;
    int gl_y = y + 2, gl_h = height * GLARE_PCT / 100;   // era y + 1: lascia respirare il bordo
    if (gl_h < 3) gl_h = 3;
    int gl_rad = (rad > 2) ? rad - 1 : 1;

    

    int cxp = x + width / 2, halfw = width / 2 + 1;
    int bl_y = y + height * VEIL_TOP / 100;
    int bl_h = (y + height * VEIL_BOT / 100) - bl_y;
    if (bl_h < 2) bl_h = 2;
    int feat  = 255 * VEIL_FEAT / 100;
    int featb = 255 * VEIL_FEB / 100;
    int inner_w = halfw * 70 / 100;

    for (int row = 0; row < height; row++) {
        int py = y + row;
        int t  = row * 100 / (height - 1);

        // Profilo misurato sul bottone Aqua: scende presto alla vita,
        // poi risale a lungo fino al picco, infine cala verso il bordo basso.
        int lum;
        if (t <= WAIST) {
            lum = -(t * DIP / WAIST);
        } else if (t <= PEAK) {
            lum = -DIP + (DIP + LIFT) * (t - WAIST) / (PEAK - WAIST);
        } else {
            lum = LIFT - (LIFT - ENDL) * (t - PEAK) / (100 - PEAK);
        }

        int r, g, b;
        if (lum < 0) {
            int d = -lum;
            r = br - br * d / 100; g = bg - bg * d / 100; b = bb - bb * d / 100;
        } else {
            r = br + (255 - br) * lum / 100;
            g = bg + (255 - bg) * lum / 100;
            b = bb + (255 - bb) * lum / 100;
        }
        uint32_t body = 0xFF000000 | (r << 16) | (g << 8) | b;

        for (int px = x; px < x + width; px++) {
            int cov = rrect_coverage(px, py, x, y, width, height, rad);
            if (!cov) continue;

            int inner = rrect_coverage(px, py, x + 1, y + 1, width - 2, height - 2, rad - 1);
            // Corpo traslucido: lo sfondo traspare appena, come nel vetro vero
            blend_pixel(px, py, body, cov * OPACITY / 255);

            // Bordo antialiasato: sfuma verso l'interno invece di tagliare netto
            int rim_a = 255 - inner;
            if (rim_a > 0)
                blend_pixel(px, py, rim, cov * rim_a / 255 * 246 / 255);

            if (inner == 0) continue;          // pieno bordo: niente riflessi
            int lit = cov * inner / 255;       // copertura utile per le luci interne

            // VELO: chiarezza diffusa nella fascia medio-bassa, sfumata su ogni lato
            if (py >= bl_y && py < bl_y + bl_h) {
                int u = (py - bl_y) * 255 / bl_h;      // 0 in alto, 255 in fondo al velo

                int vf;
                if (u < feat)              vf = u * 255 / feat;             // entra
                else if (u > 255 - featb)  vf = (255 - u) * 255 / featb;    // esce
                else                       vf = 255;                        // pieno
                vf = smooth255(vf);

                int hd = (px > cxp) ? (px - cxp) : (cxp - px);
                int hf = (hd <= inner_w) ? 255
                       : 255 - ((hd - inner_w) * 255 / (halfw - inner_w));
                if (hf > 0) {
                    hf = smooth255(hf);
                    blend_pixel(px, py, bloom_col, VEIL * vf / 255 * hf / 255 * lit / 255);
                }
            }

            // RIFLESSO: lente arrotondata in alto. Resta a piena luce nella parte
            // superiore e sfuma solo verso il bordo, come una lastra di vetro.
            if (py < gl_y + gl_h) {
                int gc = rrect_coverage(px, py, gl_x, gl_y, gl_w, gl_h, gl_rad);
                if (gc) {
                    int d = (py - gl_y) * 255 / gl_h;   // 0 in cima, 255 in fondo alla lente
                    int f;
                    if (d <= GLARE_TOP) f = 255;                                  // plateau
                    else f = 255 - ((d - GLARE_TOP) * 255 / (255 - GLARE_TOP));   // discesa
                    f = f * f / 255;                    // ammorbidisce solo la coda
                    blend_pixel(px, py, 0xFFFFFFFF, GLARE * f / 255 * gc / 255 * lit / 255);
                }
            }

            // PREMUTO: ombra interna in cima, come vetro che affonda nell'incavo
            if (pressed && py < y + 4) {
                int f = 255 - ((py - y) * 255 / 4);
                blend_pixel(px, py, rim, 70 * f / 255 * lit / 255);
            }
        }
    }
}



// LOGO "Third Millennium OS": Sole + riflesso (proporzioni auree).
// Indici: 0=Trasparente, 1=#2E86AB (orizzonte + L1), 2=#4FA6C9 (sole), 3=#59AECE (L2)
const uint8_t os_logo_bitmap[16][24] = {
    {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0}, // Y=0
    {0,0,0,0,0,0,0,0,0,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0}, // Y=1
    {0,0,0,0,0,0,0,0,0,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0}, // Y=2
    {0,0,0,0,0,0,1,0,0,1,0,0,0,0,2,2,2,2,0,0,0,0,0,0}, // Y=3
    {0,0,0,0,0,0,1,0,0,1,0,0,2,2,2,2,2,2,2,2,0,0,0,0}, // Y=4
    {0,0,0,3,3,0,1,0,0,1,0,0,2,2,2,2,2,2,2,2,0,0,0,0}, // Y=5
    {0,0,0,3,3,0,1,0,0,1,0,2,2,2,2,2,2,2,2,2,2,0,0,0}, // Y=6
    {0,0,0,3,3,0,1,0,0,1,0,2,2,2,2,2,2,2,2,2,2,0,0,0}, // Y=7
    {0,0,0,3,3,0,1,0,0,1,0,2,2,2,2,2,2,2,2,2,2,0,0,0}, // Y=8
    {0,0,0,3,3,0,1,0,0,1,0,2,2,2,2,2,2,2,2,2,2,0,0,0}, // Y=9
    {0,0,0,3,3,0,1,0,0,1,0,0,2,2,2,2,2,2,2,2,0,0,0,0}, // Y=10
    {0,0,0,0,0,0,1,0,0,1,0,0,2,2,2,2,2,2,2,2,0,0,0,0}, // Y=11
    {0,0,0,0,0,0,1,0,0,1,0,0,0,0,2,2,2,2,0,0,0,0,0,0}, // Y=12
    {0,0,0,0,0,0,0,0,0,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0}, // Y=13
    {0,0,0,0,0,0,0,0,0,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0}, // Y=14
    {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0}  // Y=15
};



// Copertura di un disco con centro a MEZZO pixel. Il sole del logo e' centrato
// fra due colonne: forzarne il centro su un pixel intero lo sposterebbe
// visibilmente in una figura larga appena dieci pixel.
// cx2, cy2, r2 sono espressi in mezzi pixel.
static int logo_disc_cov(int px, int py, int cx2, int cy2, int r2) {
    int rr = r2 * 4;
    int hits = 0;
    for (int sy = 0; sy < 4; sy++) {
        int fy = (py * 8 + sy * 2 + 1) - cy2 * 4;
        for (int sx = 0; sx < 4; sx++) {
            int fx = (px * 8 + sx * 2 + 1) - cx2 * 4;
            if (fx * fx + fy * fy <= rr * rr) hits++;
        }
    }
    return hits * 255 / 16;
}

void draw_os_logo(int start_x, int start_y) {
    // Arcobaleno per riga, distribuito sulle 14 righe disegnate (0-13).
    // Fascia verde ridotta cosi' tutti i colori restano visibili.
    static const uint32_t rainbow[16] = {
        0xFFF20D0D, 0xFFF2400D, 0xFFF2730D, 0xFFF2A60D,
        0xFFF2D90D, 0xFFB4F20D, 0xFF2BF20D, 0xFF0DF29C,
        0xFF0DD9F2, 0xFF0D91F2, 0xFF0D4AF2, 0xFF4A0DF2,
        0xFF9C0DF2, 0xFFF20DD9, 0xFFF20DD9, 0xFFF20DD9
    };

    // Orizzonte e riflessi: linee verticali, gia' nitide. Restano dalla bitmap.
    for (int y = 0; y < 16; y++) {
        for (int x = 0; x < 24; x++) {
            uint8_t v = os_logo_bitmap[y][x];
            if (v == 1 || v == 3) draw_pixel(start_x + x, start_y + y, rainbow[y]);
        }
    }

    // Il sole e' un cerchio vero: centro (15.5, 7.5) e raggio 5, esattamente
    // la circonferenza che la bitmap approssimava a gradini.
    int cx2 = start_x * 2 + 31;
    int cy2 = start_y * 2 + 15;
    int r2  = 10;

    for (int y = 1; y < 15; y++) {
        for (int x = 9; x < 23; x++) {
            int cov = logo_disc_cov(start_x + x, start_y + y, cx2, cy2, r2);
            if (cov > 0) blend_pixel(start_x + x, start_y + y, rainbow[y], cov);
        }
    }
}


// Disegna una casella di testo "scavata" (Sunken 3D Morbido e Arrotondato)
void draw_textfield(int x, int y, int width, int height, uint32_t bg_color) {
    
    // Colori fissi per la cornice scavata
    uint32_t border_out = 0xFF808080;   // Bordo esterno principale (Ombra della finestra)
    uint32_t shadow_in = 0xFF575757;    // Ombra interna scura (Il taglio netto)
    uint32_t light_edge = 0xFFFFFFFF;   // Luce in basso a destra (Riflesso)

    // 1. Sfondo Centrale (Margine di 2px per le curve)
    draw_rect(x + 2, y + 2, width - 4, height - 4, bg_color);

    // 2. Ombra interna sfumata in alto (Effetto profondità dinamica calcolata sul colore di sfondo)
    uint32_t r = (bg_color >> 16) & 0xFF;
    uint32_t g = (bg_color >> 8) & 0xFF;
    uint32_t b = bg_color & 0xFF;
    uint32_t s1 = 0xFF000000 | ((r * 85 / 100) << 16) | ((g * 85 / 100) << 8) | (b * 85 / 100);
    uint32_t s2 = 0xFF000000 | ((r * 94 / 100) << 16) | ((g * 94 / 100) << 8) | (b * 94 / 100);
    draw_rect(x + 2, y + 2, width - 4, 1, s1);
    draw_rect(x + 2, y + 3, width - 4, 1, s2);

    // 3. Bordi Esterni (Spessore 1px, accorciati per fare spazio alla curva)
    draw_rect(x + 4, y, width - 8, 1, border_out);              // Top
    draw_rect(x, y + 4, 1, height - 8, border_out);             // Left
    draw_rect(x + 4, y + height - 1, width - 8, 1, light_edge); // Bottom
    draw_rect(x + width - 1, y + 4, 1, height - 8, light_edge); // Right

    // 4. Bordi Interni (Creano lo spessore scavato)
    draw_rect(x + 4, y + 1, width - 8, 1, shadow_in);           // Inner Top
    draw_rect(x + 1, y + 4, 1, height - 8, shadow_in);          // Inner Left
    draw_rect(x + 4, y + height - 2, width - 8, 1, bg_color);   // Inner Bottom (Fuso col fondo)
    draw_rect(x + width - 2, y + 4, 1, height - 8, bg_color);   // Inner Right (Fuso col fondo)

    // 5. Curvatura Angoli (Raccordo perfetto a 4 pixel)
    
    // Top-Left (Doppia Ombra)
    draw_pixel(x + 2, y + 1, border_out); draw_pixel(x + 3, y + 1, border_out);
    draw_pixel(x + 1, y + 2, border_out); draw_pixel(x + 1, y + 3, border_out);
    draw_pixel(x + 2, y + 2, shadow_in); draw_pixel(x + 3, y + 2, shadow_in); draw_pixel(x + 2, y + 3, shadow_in);

    // Top-Right (Il bordo scuro gira l'angolo, il chiaro inizia dopo)
    draw_pixel(x + width - 3, y + 1, border_out); draw_pixel(x + width - 4, y + 1, border_out);
    draw_pixel(x + width - 2, y + 2, border_out); draw_pixel(x + width - 2, y + 3, border_out);
    draw_pixel(x + width - 3, y + 2, shadow_in);

    // Bottom-Left (Il bordo scuro gira l'angolo verso il basso)
    draw_pixel(x + 1, y + height - 3, border_out); draw_pixel(x + 1, y + height - 4, border_out);
    draw_pixel(x + 2, y + height - 2, border_out); draw_pixel(x + 3, y + height - 2, border_out);
    draw_pixel(x + 2, y + height - 3, shadow_in);

    // Bottom-Right (Doppia Luce)
    draw_pixel(x + width - 2, y + height - 3, light_edge); draw_pixel(x + width - 2, y + height - 4, light_edge);
    draw_pixel(x + width - 3, y + height - 2, light_edge); draw_pixel(x + width - 4, y + height - 2, light_edge);
    draw_pixel(x + width - 3, y + height - 3, bg_color); // Il pixel interno si fonde dolcemente
}

// ============================================================================
// SATINATO PRECALCOLATO
// La trama del corpo delle finestre dipende solo dalla posizione del pixel
// dentro la finestra: e' sempre la stessa. Ricalcolarne l'hash per ogni pixel
// a ogni fotogramma era il costo singolo piu' alto di tutto il render.
// Qui si calcola una volta una tessera 128x128 di indici di rumore (0..6),
// e a ogni disegno si costruisce una tavolozza di 7 colori: il pixel diventa
// due letture in tabella. Il rumore non ha struttura, quindi la ripetizione
// ogni 128 pixel non si nota.
// ============================================================================
#define SATIN_N 128
static uint8_t satin_tab[SATIN_N][SATIN_N];
static int satin_ready = 0;

static void satin_build(void) {
    for (uint32_t ry = 0; ry < SATIN_N; ry++) {
        for (uint32_t rx = 0; rx < SATIN_N; rx++) {
            uint32_t n = rx + (ry * 57325);
            n = (n << 13) ^ n;
            uint32_t nn = (n * (n * n * 15731 + 789221) + 1376312589);
            satin_tab[ry][rx] = (uint8_t)(nn % 7);   // 0..6, cioe' rumore -3..+3
        }
    }
    satin_ready = 1;
}

void draw_window(int x, int y, int width, int height, int is_active) {
    
    (void)is_active; // Silenziamo il warning
    
    // COLLEGAMENTO AL TEMA: Peschiamo i colori dalla tavolozza globale active_theme
    uint32_t bg_color = active_theme.window_bg; 
    uint32_t border_tl = 0xFF454545; // Grigio Antracite (Luce in alto a sinistra)
    uint32_t border_br = 0xFF000000; // Nero puro (Ombra in basso a destra)
    uint32_t shadow = 0xFF808080;

    // ========================================================================
    // 1. Barra del Titolo ESTESA (Tematizzabile, ultra-compatta)
    // ========================================================================
    
    // I colori ora sono estratti dalle variabili indipendenti della barra del titolo
    uint32_t title_dark  = active_theme.titlebar_lines; 
    uint32_t title_light = active_theme.titlebar_base;

    // A. La primissima riga in alto (1px scuro)
    draw_rect(x + 5, y + 1, width - 10, 1, title_dark);
    
    // B. La riga chiara superiore (1px a y+2)
    draw_rect(x + 3, y + 2, width - 6, 1, title_light);
    
    // C. Sfondo della barra (Margini chiari)
    draw_rect(x + 1, y + 3, width - 2, 27, title_light); 

    // D. Disegniamo le linee SCURE (intarsi, passo 6px)
    for (int ly = y + 3; ly < y + 30; ly += 6) {
        int h = 4; 
        if (ly + h > y + 30) h = (y + 30) - ly; // Sicurezza fondo
        
        // Smussatura della prima barra scura per seguire la curva della finestra
        if (ly == y + 3) {
            draw_rect(x + 4, ly, width - 8, 1, title_dark);         
            draw_rect(x + 3, ly + 1, width - 6, h - 1, title_dark); 
        } else {
            draw_rect(x + 3, ly, width - 6, h, title_dark);
        }
    }

    // ========================================================================
    // 1.5 EFFETTO LUCE GRADIENTE (Post-Processing Matematico Universale)
    // ========================================================================
    for (int py = y + 1; py <= y + 18; py++) {
        if (py < 0 || py >= fb_height) continue; 

        int white_mix = 26 - ((py - (y + 1)) * 26 / 18); 
        
        if (white_mix > 0) {
            int start_px = x + 1;
            int end_px = x + width - 1;
            
            if (py == y + 1) { start_px = x + 5; end_px = x + width - 5; }
            else if (py == y + 2) { start_px = x + 3; end_px = x + width - 3; }

            if (start_px < 0) start_px = 0;
            if (end_px > fb_width) end_px = fb_width;

            uint32_t* row_ptr = &backbuffer[py * fb_width];
            
            for (int px = start_px; px < end_px; px++) {
                uint32_t cp = row_ptr[px];
                
                uint32_t cr = (cp >> 16) & 0xFF;
                uint32_t cg = (cp >> 8) & 0xFF;
                uint32_t cb = cp & 0xFF;
                
                uint32_t r = cr + ((255 - cr) * white_mix / 100);
                uint32_t g = cg + ((255 - cg) * white_mix / 100);
                uint32_t b = cb + ((255 - cb) * white_mix / 100);
                
                row_ptr[px] = 0xFF000000 | (r << 16) | (g << 8) | b;
            }
        }
    }

    // ========================================================================
    // 2. Separatore orizzontale
    // ========================================================================
    draw_rect(x + 1, y + 30, width - 2, 2, shadow);

    // ========================================================================
    // 3. Sfondo Centrale (Texture Satinata)
    // ========================================================================
    uint32_t br = (bg_color >> 16) & 0xFF;
    uint32_t bg = (bg_color >> 8) & 0xFF;
    uint32_t bb = bg_color & 0xFF;

    int sx = x + 1; if (sx < 0) sx = 0;
    int sy = y + 32; if (sy < 0) sy = 0;
    int ex = x + width - 1; if (ex > fb_width) ex = fb_width;
    int ey = y + height - 1; if (ey > fb_height) ey = fb_height;

    if (!satin_ready) satin_build();

    // Sette colori possibili, uno per livello di rumore: calcolati qui una
    // volta sola invece che per ogni pixel.
    uint32_t pal[7];
    for (int k = 0; k < 7; k++) {
        int sh = (k - 3) * 6;
        int r = (int)br + sh, g = (int)bg + sh, b = (int)bb + sh;
        if (r < 0) r = 0; 
        if (r > 255) r = 255;
        if (g < 0) g = 0; 
        if (g > 255) g = 255;
        if (b < 0) b = 0; 
        if (b > 255) b = 255;
        pal[k] = 0xFF000000 | ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
    }

    for (int py = sy; py < ey; py++) {
        uint32_t* row_ptr = &backbuffer[py * fb_width];
        const uint8_t* trow = satin_tab[(py - y) & (SATIN_N - 1)];
        for (int px = sx; px < ex; px++)
            row_ptr[px] = pal[trow[(px - x) & (SATIN_N - 1)]];
    }

    // ========================================================================
    // 4. Bordi Esterni a 1 PIXEL (Spigoli vivi in basso, morbidi in alto)
    // ========================================================================
    draw_rect(x + 5, y, width - 10, 1, border_tl);              // Top
    draw_rect(x, y + 5, 1, height - 5, border_tl);              // Left
    draw_rect(x + width - 1, y + 5, 1, height - 5, border_br);  // Right
    draw_rect(x + 1, y + height - 1, width - 2, 1, border_br);  // Bottom

    // Angolo Top-Left 
    draw_pixel(x + 4, y + 1, border_tl);
    draw_pixel(x + 3, y + 1, border_tl);
    draw_pixel(x + 2, y + 2, border_tl);
    draw_pixel(x + 1, y + 3, border_tl);
    draw_pixel(x + 1, y + 4, border_tl);

    // Angolo Top-Right
    draw_pixel(x + width - 5, y + 1, border_tl);
    draw_pixel(x + width - 4, y + 1, border_tl);
    draw_pixel(x + width - 3, y + 2, border_tl);
    draw_pixel(x + width - 2, y + 3, border_br); 
    draw_pixel(x + width - 2, y + 4, border_br);
}

void draw_char(char c, int x, int y, uint32_t color, int scale) {
    const uint8_t *glyph = 0;
    unsigned char uc = (unsigned char)c;

    // OTTIMIZZAZIONE 1: Non disegniamo il vuoto. Lo sfondo è già stato colorato.
    if (c == ' ') return; 

    if (c >= 'A' && c <= 'Z') glyph = font8x8_A_Z[c - 'A'];
    else if (c >= 'a' && c <= 'z') glyph = font8x8_a_z[c - 'a'];  
    else if (c >= '0' && c <= '9') glyph = font8x8_numbers[c - '0'];
    else if (uc == 224) { static const uint8_t g[8] = {0x10,0x08,0x3C,0x06,0x3E,0x66,0x3B,0x00}; glyph = g; } 
    else if (uc == 232) { static const uint8_t g[8] = {0x10,0x08,0x3C,0x66,0x7E,0x60,0x3C,0x00}; glyph = g; } 
    else if (uc == 233) { static const uint8_t g[8] = {0x08,0x10,0x3C,0x66,0x7E,0x60,0x3C,0x00}; glyph = g; } 
    else if (uc == 236) { static const uint8_t g[8] = {0x20,0x10,0x00,0x38,0x18,0x18,0x3C,0x00}; glyph = g; } 
    else if (uc == 242) { static const uint8_t g[8] = {0x10,0x08,0x3C,0x66,0x66,0x66,0x3C,0x00}; glyph = g; } 
    else if (uc == 249) { static const uint8_t g[8] = {0x10,0x08,0x66,0x66,0x66,0x66,0x3B,0x00}; glyph = g; } 
    else if (c == '-') { static const uint8_t g[8] = {0,0,0,0x3C,0,0,0,0}; glyph = g; }
    else if (c == '>') { static const uint8_t g[8] = {0x00, 0x40, 0x20, 0x10, 0x20, 0x40, 0x00, 0x00}; glyph = g; }
    else if (c == '<') { static const uint8_t g[8] = {0x00, 0x10, 0x20, 0x40, 0x20, 0x10, 0x00, 0x00}; glyph = g; }
    else if (c == ':') { static const uint8_t g[8] = {0,0,0x18,0x18,0,0x18,0x18,0}; glyph = g; }
    else if (c == ';') { static const uint8_t g[8] = {0,0,0x18,0x18,0,0x18,0x18,0x30}; glyph = g; }
    else if (c == '.') { static const uint8_t g[8] = {0,0,0,0,0,0x18,0x18,0}; glyph = g; }
    else if (c == ',') { static const uint8_t g[8] = {0,0,0,0,0,0x18,0x18,0x30}; glyph = g; }
    else if (c == '(') { static const uint8_t g[8] = {0x0C,0x18,0x30,0x30,0x30,0x18,0x0C,0}; glyph = g; }
    else if (c == ')') { static const uint8_t g[8] = {0x30,0x18,0x0C,0x0C,0x0C,0x18,0x30,0}; glyph = g; }
    else if (c == '[') { static const uint8_t g[8] = {0x3C,0x30,0x30,0x30,0x30,0x30,0x3C,0}; glyph = g; }
    else if (c == ']') { static const uint8_t g[8] = {0x3C,0x0C,0x0C,0x0C,0x0C,0x0C,0x3C,0}; glyph = g; }
    else if (c == '{') { static const uint8_t g[8] = {0x18,0x30,0x30,0x60,0x30,0x30,0x18,0}; glyph = g; }
    else if (c == '}') { static const uint8_t g[8] = {0x18,0x0C,0x0C,0x06,0x0C,0x0C,0x18,0}; glyph = g; }
    else if (c == '/') { static const uint8_t g[8] = {0x00,0x02,0x04,0x08,0x10,0x20,0x40,0x00}; glyph = g; }
    else if (c == '\\') { static const uint8_t g[8] = {0x00,0x80,0x40,0x20,0x10,0x08,0x04,0x00}; glyph = g; }
    else if (c == '?') { static const uint8_t g[8] = {0x3C,0x66,0x06,0x0C,0x18,0x00,0x18,0x00}; glyph = g; }
    else if (c == '!') { static const uint8_t g[8] = {0x18,0x18,0x18,0x18,0x18,0x00,0x18,0x00}; glyph = g; }
    else if (c == '\'') { static const uint8_t g[8] = {0x18,0x18,0x08,0,0,0,0,0}; glyph = g; }
    else if (c == '"')  { static const uint8_t g[8] = {0x36,0x36,0x12,0,0,0,0,0}; glyph = g; }
    else if (c == '_')  { static const uint8_t g[8] = {0,0,0,0,0,0,0,0x7E}; glyph = g; }
    else if (c == '=')  { static const uint8_t g[8] = {0,0,0x7E,0,0x7E,0,0,0}; glyph = g; }
    else if (c == '+')  { static const uint8_t g[8] = {0,0x18,0x18,0x7E,0x18,0x18,0,0}; glyph = g; }
    else if (c == '*')  { static const uint8_t g[8] = {0,0x00,0x54,0x38,0x7C,0x38,0x54,0x00}; glyph = g; }
    else if (c == '%')  { static const uint8_t g[8] = {0x23,0x23,0x10,0x08,0x04,0x62,0x62,0}; glyph = g; }
    else if (c == '|')  { static const uint8_t g[8] = {0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x18}; glyph = g; }
    else return;

    if (scale == 1) {
        // === OTTIMIZZAZIONE 2: FAST PATH (Bypassa draw_rect) ===
        for (int row = 0; row < 8; row++) {
            int py = y + row;
            if (py < 0 || py >= fb_height) continue;
            
            uint8_t glyph_row = glyph[row];
            if (glyph_row == 0) continue; // Salta intere righe vuote del font.
            
            uint32_t* row_ptr = &backbuffer[py * fb_width];
            for (int col = 0; col < 8; col++) {
                int px = x + col;
                if (px >= 0 && px < fb_width) {
                    if (glyph_row & (1 << (7 - col))) {
                        row_ptr[px] = color;
                    }
                }
            }
        }
    } else {
        // === SLOW PATH: Usato solo per i titoli delle finestre (scale = 2) ===
        for (int row = 0; row < 8; row++) {
            for (int col = 0; col < 8; col++) {
                if (glyph[row] & (1 << (7 - col))) {
                    draw_rect(x + (col * scale), y + (row * scale), scale, scale, color);
                }
            }
        }
    }
}

void draw_string(const char *str, int x, int y, uint32_t color, int scale) {
    int cursor_x = x;
    while (*str != '\0') {
        draw_char(*str, cursor_x, y, color, scale);
        cursor_x += 8 * scale; // Spostiamo il cursore in base alla grandezza
        str++;
    }
}

// Calcola la larghezza totale (Ora il font è nativamente perfetto, niente scaling)
int get_prop_string_width(const char* str) {
    int width = 0;
    while (*str != '\0') {
        unsigned char uc = (unsigned char)*str;
        if (FONT_HAS(uc)) {
            if (uc == ' ') width += 5; // Spazio largo 5px
            else width += font_prop[uc].width + 1; // 1px di respiro naturale
        } else width += 8; 
        str++;
    }
    return width;
}

// Stampa la stringa in Alta Risoluzione (16px di altezza)
void draw_prop_string(const char *str, int x, int y, uint32_t color) {
    int cursor_x = x;
    
    while (*str != '\0') {
        unsigned char uc = (unsigned char)*str;
        
        if (!FONT_HAS(uc)) {
            draw_char(*str, cursor_x, y, color, 1);
            cursor_x += 8;
        } 
        else if (uc == ' ') {
            cursor_x += 5; // Spazio vuoto
        }
        else {
            uint8_t width = font_prop[uc].width;
            if (width > 0) {
                // IL CICLO ORA LEGGE 16 RIGHE DI ALTEZZA
                for (int row = 0; row < 16; row++) {
                    uint8_t glyph_row = font_prop[uc].glyph[row];
                    if (glyph_row == 0) continue;
                    
                    for (int col = 0; col < width; col++) {
                        
                        if (glyph_row & (1 << (7 - col))) {
                            draw_pixel(cursor_x + col, y + row, color);
                        }
                    }
                }
            }
            cursor_x += width + 1; // Avanziamo per la prossima lettera
        }
        str++;
    }
}


// Calcola la larghezza della stringa in modalità GRASSETTO
int get_prop_string_width_bold(const char* str) {
    int width = 0;
    while (*str != '\0') {
        unsigned char uc = (unsigned char)*str;
        if (FONT_HAS(uc)) {
            if (uc == ' ') width += 6; // Spazio più largo (6px)
            else width += font_prop[uc].width + 2; // +1 di respiro, +1 di ingombro grassetto
        } else width += 9; 
        str++;
    }
    return width;
}

// Stampa la stringa in Alta Risoluzione GRASSETTO (Synthetic Bold)
void draw_prop_string_bold(const char *str, int x, int y, uint32_t color) {
    int cursor_x = x;
    
    while (*str != '\0') {
        unsigned char uc = (unsigned char)*str;
        
        if (!FONT_HAS(uc)) {
            draw_char(*str, cursor_x, y, color, 1);
            draw_char(*str, cursor_x + 1, y, color, 1); // Sovrascrittura per grassetto finto
            cursor_x += 9;
        } 
        else if (uc == ' ') {
            cursor_x += 6; // Spazio vuoto allargato
        }
        else {
            uint8_t width = font_prop[uc].width;
            if (width > 0) {
                for (int row = 0; row < 16; row++) {
                    uint8_t glyph_row = font_prop[uc].glyph[row];
                    if (glyph_row == 0) continue;
                    
                    for (int col = 0; col < width; col++) {
                        if (glyph_row & (1 << (7 - col))) {
                            draw_pixel(cursor_x + col, y + row, color);
                            draw_pixel(cursor_x + col + 1, y + row, color); // BOLD
                        }
                    }
                }
            }
            cursor_x += width + 2; // Avanziamo tenendo conto dell'ispessimento
        }
        str++;
    }
}

// ----------------------------------------------------------------------------
// Ombra morbida del testo.
// Costruisce prima una maschera (prendendo il massimo, non sommando), poi
// miscela UNA sola volta per pixel: cosi' l'opacita' non si accumula sui
// tratti spessi e 'alpha' corrisponde davvero all'intensita' finale.
// Va disegnata PRIMA del testo vero.
// ----------------------------------------------------------------------------
#define TSH_W   512
#define TSH_H   24
#define TSH_OFF 1        // margine a sinistra per l'alone
static uint8_t tsh_mask[TSH_H][TSH_W];

void draw_prop_string_bold_shadow(const char *str, int x, int y, int dy, uint32_t color, int alpha) {
    int wpx = get_prop_string_width_bold(str) + 4;
    if (wpx <= 0) return;
    if (wpx > TSH_W) wpx = TSH_W;

    for (int r = 0; r < TSH_H; r++)
        for (int c = 0; c < wpx; c++) tsh_mask[r][c] = 0;

    const char *p = str;
    int cur = TSH_OFF;

    while (*p != '\0') {
        unsigned char uc = (unsigned char)*p;

        if (!FONT_HAS(uc))      { cur += 9; p++; continue; }
        else if (uc == ' ') { cur += 6; p++; continue; }

        uint8_t width = font_prop[uc].width;
        if (width > 0) {
            for (int row = 0; row < 16; row++) {
                uint8_t gr = font_prop[uc].glyph[row];
                if (gr == 0) continue;

                for (int col = 0; col < width; col++) {
                    if (!(gr & (1 << (7 - col)))) continue;
                    int cx = cur + col;

                    // Nucleo (segue anche l'ispessimento del grassetto)
                    for (int k = 0; k <= 1; k++) {
                        int mx = cx + k;
                        if (mx >= 0 && mx < wpx) tsh_mask[row][mx] = 255;
                    }

                    // Alone tenue attorno, senza mai sovrascrivere il nucleo
                    int hx[4] = { cx - 1, cx + 2, cx,       cx + 1   };
                    int hy[4] = { row,    row,    row + 1,  row + 1  };
                    for (int k = 0; k < 4; k++) {
                        int mx = hx[k], my = hy[k];
                        if (mx < 0 || mx >= wpx || my < 0 || my >= TSH_H) continue;
                        if (tsh_mask[my][mx] < 105) tsh_mask[my][mx] = 105;
                    }
                }
            }
        }
        cur += width + 2;
        p++;
    }

    for (int r = 0; r < TSH_H; r++) {
        for (int c = 0; c < wpx; c++) {
            int m = tsh_mask[r][c];
            if (m == 0) continue;
            blend_pixel(x + c - TSH_OFF, y + dy + r, color, alpha * m / 255);
        }
    }
}

// Stampa la stringa tagliandola perfettamente dentro un rettangolo (Clipping) - realizzata per le textbox
void draw_prop_string_clipped(const char *str, int x, int y, uint32_t color, int cx, int cy, int cw, int ch) {
    int cursor_x = x;
    int c_end_x = cx + cw;
    int c_end_y = cy + ch;
    
    while (*str != '\0') {
        unsigned char uc = (unsigned char)*str;
        
        if (!FONT_HAS(uc)) {
            cursor_x += 8; // Ignoriamo i caratteri speciali estesi nel clipping per ora
        } 
        else if (uc == ' ') {
            cursor_x += 5; // Spazio vuoto
        }
        else {
            uint8_t width = font_prop[uc].width;
            if (width > 0) {
                for (int row = 0; row < 16; row++) {
                    uint8_t glyph_row = font_prop[uc].glyph[row];
                    if (glyph_row == 0) continue;
                    
                    for (int col = 0; col < width; col++) {
                        if (glyph_row & (1 << (7 - col))) {
                            int px = cursor_x + col;
                            int py = y + row;
                            
                            // ==========================================
                            // LA MAGIA DEL CLIPPING PIXEL-PERFECT
                            // ==========================================
                            if (px >= cx && px < c_end_x && py >= cy && py < c_end_y) {
                                draw_pixel(px, py, color);
                            }
                        }
                    }
                }
            }
            cursor_x += width + 1; // Avanziamo
        }
        str++;
    }
}


// ==============================================================================
// ASSET GRAFICI UI PERSONALIZZATI (Risoluzione 16x16 Pixel-Perfect)
// ==============================================================================

// Matrice 16x16: La "X" per chiudere (Perfettamente centrata, 10x10 px)
const uint16_t ui_glyph_close[16] = {
0x0000, 0x0000, 0x0000, 0x0000, // Margine Top (4px vuoti, gambe più corte)
    0x1C38, // ...XXX....XXX... (Gambe larghe 3px)
    0x0E70, // ....XXX..XXX....
    0x07E0, // .....XXXXXX.....
    0x03C0, // ......XXXX...... (Centro compatto e tozzo)
    0x03C0, // ......XXXX...... 
    0x07E0, // .....XXXXXX.....
    0x0E70, // ....XXX..XXX....
    0x1C38, // ...XXX....XXX...
    0x0000, 0x0000, 0x0000, 0x0000  // Margine Bottom (4px vuoti)
};



// ========================================================
// ICONA: RIPRISTINA FINESTRA (Clessidra Ruotata ▶◀)
// ========================================================
const uint16_t ui_glyph_restore[16] = {
    0x0000, 0x0000, 0x0000, 0x0000, // 4px di margine Top (Esattamente come la X)
    0x1818, // ...XX......XX...  (Basi esterne piatte)
    0x1C38, // ...XXX....XXX...  (I triangoli si allargano)
    0x1E78, // ...XXXX..XXXX...  (Punte vicinissime: 2px di spazio)
    0x1FF8, // ...XXXXXXXXXX...  (Punte unite al centro)
    0x1FF8, // ...XXXXXXXXXX...  (Punte unite al centro)
    0x1E78, // ...XXXX..XXXX...  (Punte vicinissime)
    0x1C38, // ...XXX....XXX...  (I triangoli si stringono)
    0x1818, // ...XX......XX...  (Basi esterne piatte)
    0x0000, 0x0000, 0x0000, 0x0000  // 4px di margine Bottom
};

// Matrice 16x16: Il Triangolo (Perfettamente centrato, base 12px, altezza 6px)
const uint16_t ui_glyph_maximize[16] = {
    0x0000, 0x0000, 0x0000, 0x0000, 0x0000, // Margine Top profondo (5px vuoti)
    0x0180, // .......XX.......  (Punta lontana dal bordo)
    0x03C0, // ......XXXX......
    0x07E0, // .....XXXXXX.....
    0x0FF0, // ....XXXXXXXX....
    0x1FF8, // ...XXXXXXXXXX...
    0x3FFC, // ..XXXXXXXXXXXX..  (Base non tocca i bordi laterali)
    0x0000, 0x0000, 0x0000, 0x0000, 0x0000  // Margine Bottom profondo (5px vuoti)
};

// Matrice 16x16: Il Triangolo Inverso per la Riduzione a Icona
const uint16_t ui_glyph_minimize[16] = {
    0x0000, 0x0000, 0x0000, 0x0000, 0x0000, // Margine Top profondo (5px vuoti)
    0x3FFC, // ..XXXXXXXXXXXX..  (Base in alto)
    0x1FF8, // ...XXXXXXXXXX...
    0x0FF0, // ....XXXXXXXX....
    0x07E0, // .....XXXXXX.....
    0x03C0, // ......XXXX......
    0x0180, // .......XX.......  (Punta in basso)
    0x0000, 0x0000, 0x0000, 0x0000, 0x0000  // Margine Bottom profondo (5px vuoti)
};

// ==============================================================================
// MOTORE ICONE DI SISTEMA (16x16 px a 16 Colori)
// ==============================================================================

// La Palette Standard di Ante-Millennium OS
const uint32_t icon_palette[32] = {
    0x00000000, // 0: Trasparente (NON DISEGNARE)
    0xFF000000, // 1: Nero puro
    0xFF808080, // 2: Grigio Scuro
    0xFFC0C0C0, // 3: Grigio Chiaro (Argento)
    0xFFFFFFFF, // 4: Bianco puro
    0xFF800000, // 5: Rosso Scuro
    0xFFFF0000, // 6: Rosso Acceso
    0xFF008000, // 7: Verde Scuro
    0xFF00FF00, // 8: Verde Acceso
    0xFF000080, // 9: Blu Scuro
    0xFF0000FF, // A(10): Blu Acceso
    0xFF808000, // B(11): Giallo Scuro (Oliva)
    0xFFFFFF00, // C(12): Giallo Acceso
    0xFF800080, // D(13): Viola Scuro
    0xFFFF00FF, // E(14): Magenta Acceso
    0xFF00FFFF, // F(15): Ciano (Azzurro)

    // --- Scala neutra (grafite fredda), intonata al vetro del sistema ---
    // Per le icone di nuovo stile: stanno bene con qualunque tema.
    0xFF2E3238, // 16: contorno grafite scuro
    0xFF4A5059, // 17: grafite
    0xFF6B727C, // 18: ardesia
    0xFF8E959E, // 19: grigio medio
    0xFFB2B8BF, // 20: argento
    0xFFD2D6DB, // 21: argento chiaro
    0xFFEEF1F4, // 22: riflesso
    0xFF000000, 0xFF000000, 0xFF000000, 0xFF000000, 0xFF000000,   // 23-27: liberi
    0xFF000000, 0xFF000000, 0xFF000000, 0xFF000000                // 28-31: liberi
};

// L'Archivio di Sistema (MAX 10 Icone predefinite)
// Ogni icona è formata da 16 righe di 16 numeri (da 0 a 15)
const uint8_t sys_icons[10][16][16] = {
    
    // ICONA 0: DEFAULT (Finestra Generica)
    {
        {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
        {0,1,1,1,1,1,1,1,1,1,1,1,1,1,1,0},
        {0,1,9,9,9,9,9,9,9,9,9,9,9,9,1,0},
        {0,1,9,9,9,9,9,9,9,9,9,9,9,9,1,0},
        {0,1,1,1,1,1,1,1,1,1,1,1,1,1,1,0},
        {0,1,4,4,4,4,4,4,4,4,4,4,4,4,1,0},
        {0,1,4,4,4,4,4,4,4,4,4,4,4,4,1,0},
        {0,1,4,4,4,4,4,4,4,4,4,4,4,4,1,0},
        {0,1,4,4,4,4,4,4,4,4,4,4,4,4,1,0},
        {0,1,4,4,4,4,4,4,4,4,4,4,4,4,1,0},
        {0,1,4,4,4,4,4,4,4,4,4,4,4,4,1,0},
        {0,1,4,4,4,4,4,4,4,4,4,4,4,4,1,0},
        {0,1,4,4,4,4,4,4,4,4,4,4,4,4,1,0},
        {0,1,1,1,1,1,1,1,1,1,1,1,1,1,1,0},
        {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
        {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0}
    },
    
    // ICONA 1: IL TERMINALE (Schermo Nero con Cursore Verde)
    {
        {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
        {0,1,1,1,1,1,1,1,1,1,1,1,1,1,1,0},
        {1,2,2,2,2,2,2,2,2,2,2,2,2,2,2,1},
        {1,2,3,3,3,3,3,3,3,3,3,3,3,3,2,1},
        {1,2,1,1,1,1,1,1,1,1,1,1,1,2,2,1},
        {1,2,1,8,1,1,1,1,1,1,1,1,1,2,2,1},
        {1,2,1,1,1,1,1,1,1,1,1,1,1,2,2,1},
        {1,2,1,1,1,1,1,1,1,1,1,1,1,2,2,1},
        {1,2,1,1,1,1,1,1,1,1,1,1,1,2,2,1},
        {1,2,1,1,1,1,1,1,1,1,1,1,1,2,2,1},
        {1,2,1,1,1,1,1,1,1,1,1,1,1,2,2,1},
        {1,2,3,3,3,3,3,3,3,3,3,3,3,3,2,1},
        {1,2,2,2,2,2,2,2,2,2,2,2,2,2,2,1},
        {0,1,1,1,1,1,1,1,1,1,1,1,1,1,1,0},
        {0,0,0,0,1,2,2,2,2,2,2,1,0,0,0,0},
        {0,0,0,1,2,2,2,2,2,2,2,2,1,0,0,0}
    },

    // ICONA 2: FILE MANAGER (Cartella argento, scala neutra)
    // Fondo ardesia, lembo anteriore con un riflesso in cima, bordo grafite.
    {
        {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
        {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
        {0,16,16,16,16,16,0,0,0,0,0,0,0,0,0,0},
        {16,19,19,19,19,19,16,0,0,0,0,0,0,0,0,0},
        {16,19,19,19,19,19,19,16,16,16,16,16,16,16,0,0},
        {16,19,19,19,19,19,19,19,19,19,19,19,19,19,16,0},
        {16,19,18,16,16,16,16,16,16,16,16,16,16,16,16,16},
        {16,19,16,22,22,22,22,22,22,22,22,22,22,22,22,16},
        {16,19,16,21,21,21,21,21,21,21,21,21,21,21,21,16},
        {16,19,16,20,20,20,20,20,20,20,20,20,20,20,20,16},
        {16,19,16,20,20,20,20,20,20,20,20,20,20,20,20,16},
        {16,19,16,20,20,20,20,20,20,20,20,20,20,20,20,16},
        {16,18,16,19,19,19,19,19,19,19,19,19,19,19,19,16},
        {16,18,16,19,19,19,19,19,19,19,19,19,19,19,19,16},
        {0,16,16,16,16,16,16,16,16,16,16,16,16,16,16,16},
        {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0}
    },

    // ICONA 3: APPLICATIONS (4 Cubi Colorati)
    {
        {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
        {0,0,1,1,1,1,0,0,0,1,1,1,1,0,0,0},
        {0,1,6,6,6,6,1,0,1,8,8,8,8,1,0,0},
        {0,1,6,4,6,6,1,0,1,8,4,8,8,1,0,0},
        {0,1,6,6,6,6,1,0,1,8,8,8,8,1,0,0},
        {0,1,5,5,5,5,1,0,1,7,7,7,7,1,0,0},
        {0,0,1,1,1,1,0,0,0,1,1,1,1,0,0,0},
        {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
        {0,0,1,1,1,1,0,0,0,1,1,1,1,0,0,0},
        {0,1,10,10,10,10,1,0,1,12,12,12,12,1,0,0},
        {0,1,10,4,10,10,1,0,1,12,4,12,12,1,0,0},
        {0,1,10,10,10,10,1,0,1,12,12,12,12,1,0,0},
        {0,1,9,9,9,9,1,0,1,11,11,11,11,1,0,0},
        {0,0,1,1,1,1,0,0,0,1,1,1,1,0,0,0},
        {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
        {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0}
    },

    // ICONA 4: SETTINGS (Ingranaggio metallico, scala neutra)
    // Otto denti, mozzo forato; la luce scende dall'alto a sinistra, e il
    // bordo e' piu' morbido dal lato illuminato.
    {
        {0,0,0,0,0,0,17,17,17,0,0,0,0,0,0,0},
        {0,0,0,17,17,0,17,21,17,0,17,17,0,0,0,0},
        {0,0,17,22,22,17,21,21,21,17,20,20,16,0,0,0},
        {0,0,17,22,22,21,21,21,21,20,20,20,16,0,0,0},
        {0,0,0,17,21,21,17,17,17,20,20,16,0,0,0,0},
        {0,17,17,21,21,17,0,0,0,16,20,20,16,16,0,0},
        {17,22,21,21,17,0,0,0,0,0,16,19,19,19,16,0},
        {17,21,21,21,17,0,0,0,0,0,16,19,19,19,16,0},
        {17,21,21,21,17,0,0,0,0,0,16,19,19,18,16,0},
        {0,17,17,20,20,16,0,0,0,16,19,19,16,16,0,0},
        {0,0,0,17,20,20,16,16,16,19,19,16,0,0,0,0},
        {0,0,17,20,20,20,19,19,19,19,18,18,16,0,0,0},
        {0,0,16,20,20,16,19,19,19,16,18,18,16,0,0,0},
        {0,0,0,16,16,0,16,19,16,0,16,16,0,0,0,0},
        {0,0,0,0,0,0,16,16,16,0,0,0,0,0,0,0},
        {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0}
    },

    // ICONA 5: EXIT (Vecchio Monitor CRT con un mini Sistema Operativo disegnato dentro!)
    {
        {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
        {0,0,1,1,1,1,1,1,1,1,1,1,1,1,0,0},
        {0,1,3,3,3,3,3,3,3,3,3,3,3,3,1,0}, // Bordo superiore scocca
        {0,1,3,1,1,1,1,1,1,1,1,1,1,3,1,0}, // Cornice nera
        {0,1,3,1,15,15,15,15,15,15,15,15,1,3,1,0}, // Desktop Azzurro
        {0,1,3,1,15, 3, 3, 3,15,15,15,15,1,3,1,0}, // Barra titolo finestra
        {0,1,3,1,15, 4, 4, 4,15,15,15,15,1,3,1,0}, // Corpo della finestra bianca
        {0,1,3,1,15,15,15,15,15,15,15,15,1,3,1,0}, // Desktop Azzurro
        {0,1,3,1, 3, 3, 3, 3, 3, 3, 3, 3,1,3,1,0}, // Taskbar inferiore!
        {0,1,3,1,1,1,1,1,1,1,1,1,1,3,1,0}, // Cornice inferiore
        {0,1,3,3,3,3,3,3,3,3,3,6,3,3,1,0}, // Scocca con LED Rosso acceso
        {0,0,1,1,1,1,1,1,1,1,1,1,1,1,0,0},
        {0,0,0,0,0,1,3,3,3,1,0,0,0,0,0,0}, // Collo del monitor
        {0,0,0,0,1,3,3,3,3,3,1,0,0,0,0,0}, 
        {0,0,0,1,3,3,3,3,3,3,3,1,0,0,0,0}, // Base
        {0,0,0,1,1,1,1,1,1,1,1,1,0,0,0,0}  
    },
    // ICONA 6: LENTE D'INGRANDIMENTO (Ricerca)
    {
        {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
        {0,0,0,0,1,1,1,1,1,0,0,0,0,0,0,0},
        {0,0,0,1,15,15,15,4,4,1,0,0,0,0,0,0}, // 15=Ciano, 4=Riflesso Bianco
        {0,0,1,15,15,15,15,4,15,1,0,0,0,0,0,0},
        {0,0,1,15,15,15,15,15,15,1,0,0,0,0,0,0},
        {0,0,1,4,15,15,15,15,15,1,0,0,0,0,0,0},
        {0,0,1,4,15,15,15,15,15,1,0,0,0,0,0,0},
        {0,0,0,1,4,4,15,15,1,2,1,0,0,0,0,0}, // 2=Manico Grigio Scuro
        {0,0,0,0,1,1,1,1,1,1,2,1,0,0,0,0},
        {0,0,0,0,0,0,0,0,1,2,2,1,0,0,0,0},
        {0,0,0,0,0,0,0,0,0,1,2,2,1,0,0,0},
        {0,0,0,0,0,0,0,0,0,0,1,2,2,1,0,0},
        {0,0,0,0,0,0,0,0,0,0,0,1,2,2,1,0},
        {0,0,0,0,0,0,0,0,0,0,0,0,1,1,0,0},
        {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
        {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0}
    }
};

// La funzione che dipinge l'icona a schermo
void draw_icon_16(int icon_id, int start_x, int start_y) {
    if (icon_id < 0 || icon_id > 9) icon_id = 0; // Fallback di sicurezza
    
    for (int y = 0; y < 16; y++) {
        for (int x = 0; x < 16; x++) {
            uint8_t color_index = sys_icons[icon_id][y][x];
            if (color_index > 0) { // L'indice 0 significa trasparente!
                draw_pixel(start_x + x, start_y + y, icon_palette[color_index]);
            }
        }
    }
}



// Nuova funzione per disegnare glifi ad alta risoluzione (Senza "gonfiarli")
void draw_ui_glyph_16(const uint16_t* glyph, int x, int y, uint32_t color) {
    for (int row = 0; row < 16; row++) {
        for (int col = 0; col < 16; col++) {
            // Controlliamo ogni singolo bit da sinistra (15) a destra (0)
            if (glyph[row] & (1 << (15 - col))) {
                draw_pixel(x + col, y + row, color); 
            }
        }
    }
}


// Preset Aqua per i bottoni: velo esteso fino in fondo, corpo opaco, bordo tinto.
void gfx_style_aqua(uint32_t veil, uint32_t rim) {
    gfx_veil_bot = 104; gfx_veil_feather = 14;
    gfx_veil_tint = veil; gfx_rim_tint = rim;
    gfx_button_opacity = 255;
}

// ----------------------------------------------------------------------------
// Derivazione automatica di bordo, velo e stato premuto da un colore base.
// Le costanti sono ricavate misurando il bottone Aqua di riferimento, ma qui
// sono espresse come RELAZIONI, cosi' valgono per qualunque tinta.
// ----------------------------------------------------------------------------

// Il bordo Aqua non e' solo piu' scuro del corpo: e' anche piu' SATURO.
// Scuriamo al 45% e poi allontaniamo i canali dalla loro media.
uint32_t gfx_derive_rim(uint32_t body) {
    int r = ((body >> 16) & 0xFF) * 45 / 100;
    int g = ((body >>  8) & 0xFF) * 45 / 100;
    int b =  (body        & 0xFF) * 45 / 100;

    int m = (r + g + b) / 3;
    r = m + (r - m) * 25 / 10;
    g = m + (g - m) * 25 / 10;
    b = m + (b - m) * 25 / 10;

    if (r < 0) r = 0;
    if (r > 255) r = 255;
    if (g < 0) g = 0;
    if (g > 255) g = 255;
    if (b < 0) b = 0;
    if (b > 255) b = 255;
    return 0xFF000000 | (r << 16) | (g << 8) | b;
}

// Il velo e' il corpo portato molto in alto in luminosita' MANTENENDO la tinta:
// il canale massimo va a 255, il minimo sale del 60%, il resto si riscala.
uint32_t gfx_derive_veil(uint32_t body) {
    int r = (body >> 16) & 0xFF, g = (body >> 8) & 0xFF, b = body & 0xFF;

    int mx = r; if (g > mx) mx = g; if (b > mx) mx = b;
    int mn = r; if (g < mn) mn = g; if (b < mn) mn = b;

    int new_mn = mn + (255 - mn) * 60 / 100;
    int span = mx - mn;
    if (span == 0) return 0xFF000000 | (new_mn << 16) | (new_mn << 8) | new_mn;

    int new_span = 255 - new_mn;
    r = new_mn + (r - mn) * new_span / span;
    g = new_mn + (g - mn) * new_span / span;
    b = new_mn + (b - mn) * new_span / span;
    return 0xFF000000 | (r << 16) | (g << 8) | b;
}

// Stato premuto: 20 punti di luminanza sotto il corpo a riposo. Piu' giu' i due
// stati sembrano due colori diversi invece che due stati dello stesso bottone.
uint32_t gfx_derive_pressed(uint32_t body) {
    int r = (body >> 16) & 0xFF, g = (body >> 8) & 0xFF, b = body & 0xFF;

    int lum = (r * 30 + g * 59 + b * 11) / 100;
    if (lum < 1) lum = 1;
    int target = lum - 20;
    if (target < 8) target = 8;

    r = r * target / lum; g = g * target / lum; b = b * target / lum;
    return 0xFF000000 | (r << 16) | (g << 8) | b;
}

// Ritorno ai default storici. Le manopole sono globali: non ripristinarle
// significa far ereditare lo stile al widget disegnato subito dopo.
void gfx_style_reset(void) {
    gfx_veil_bot = 98; gfx_veil_feather = 32;
    gfx_veil_tint = 0xFFFFFFFF; gfx_rim_tint = 0;
    gfx_button_opacity = 210;
}


// ============================================================================
// RAGGI RADIALI
// ============================================================================
// L'effetto dell'icona App Store: ventagli chiari che partono dal centro e si
// aprono verso il bordo.
//
// Niente virgola mobile: il kernel gira freestanding, senza libm e con l'FPU
// non salvata dal context switch. Qui sotto c'e' un atan2 intero con
// l'approssimazione polinomiale classica, errore sotto lo 0,2%: per un effetto
// visivo e' piu' che sufficiente.
//
// Gli assi sono normalizzati sulla semilarghezza e sulla semialtezza prima di
// calcolare l'angolo. Con l'angolo geometrico vero, su un bottone largo e basso
// i raggi si ammasserebbero a sinistra e a destra lasciando vuoti sopra e sotto.

// atan(z) per z in [0,1] espresso come r/1024. Restituisce 0..512, dove 512
// corrisponde a pi/4. Approssimazione di Rajan: (pi/4)z - z(z-1)(0.2447+0.0663z)
static int atan_unit(int r) {
    int t1 = (512 * r) / 1024;
    int zz = (r * (r - 1024)) / 1024;        // z(z-1) * 1024, negativo
    int k  = 2447 + (663 * r) / 1024;        // (0.2447 + 0.0663z) * 1e4
    int t2 = ((zz * k) / 1024) * 652 / 10000;
    return t1 - t2;
}

// Giro completo mappato su 0..4095.
static int iatan2(int y, int x) {
    if (x == 0 && y == 0) return 0;
    int ax = x < 0 ? -x : x;
    int ay = y < 0 ? -y : y;
    int a;

    if (ax >= ay) a = atan_unit((ay * 1024) / ax);
    else          a = 1024 - atan_unit((ax * 1024) / ay);

    if (x < 0) a = 2048 - a;
    if (y < 0) a = 4096 - a;
    return a & 4095;
}

// count    = numero di raggi. RAYS_AUTO (0) lo ricava dalla larghezza.
// strength = alpha massimo, 0-255. Sotto i 60 resta discreto.
// color    = tinta dei raggi. Il bianco schiarisce senza spostare la tinta;
//            un colore saturo va tenuto piu' basso di alpha, perche' tinge.
void draw_button_rays(int x, int y, int w, int h,
                      int count, int strength, uint32_t color) {
    if (count <= 0) {
        count = 12 + w / 12;
        if (count > 32) count = 32;
    }

    // Il ritaglio deve usare lo STESSO raggio di draw_button(), manopola
    // compresa, altrimenti su un widget squadrato i raggi sborderebbero.
    const int RAD_MAX = 10;
    int rad;
    if (gfx_button_radius >= 0) rad = gfx_button_radius;
    else { rad = h / 2; if (rad > RAD_MAX) rad = RAD_MAX; }
    if (rad > w / 2) rad = w / 2;

    int hw = w / 2, hh = h / 2;
    if (hw < 1 || hh < 1) return;
    int cx = x + hw, cy = y + hh;

    for (int py = y; py < y + h; py++) {
        for (int px = x; px < x + w; px++) {
            // Stessa copertura antialiasata del bottone: i raggi si fermano
            // sul bordo senza scalettature.
            int cov = rrect_coverage(px, py, x, y, w, h, rad);
            if (cov <= 0) continue;

            int dx = ((px - cx) * 1024) / hw;
            int dy = ((py - cy) * 1024) / hh;

            int a = iatan2(dy, dx);

            // Onda triangolare: count cicli sul giro completo.
            int p = (a * count) & 4095;
            int wave = (p < 2048) ? p : 4096 - p;    // 0..2048
            wave = wave * 255 / 2048;
            wave = wave * wave / 255;                 // punte piu' strette

            // Vicino al centro gli angoli si affollano: senza questa dissolvenza
            // il vertice diventa un rumore di moire' invece che un fuoco.
            int dist = (dx < 0 ? -dx : dx) + (dy < 0 ? -dy : dy);
            if (dist < 220) wave = wave * dist / 220;

            int alpha = strength * wave / 255 * cov / 255;
            if (alpha > 0) blend_pixel(px, py, color, alpha);
        }
    }
}

// ============================================================================
// BAGLIORE A RAGGI ASCENDENTE
// ============================================================================
// Stessa matematica dei raggi dei bottoni, ma con la sorgente sotto il riquadro:
// il ventaglio si apre verso l'alto invece che in tutte le direzioni.
// Oltre ai raggi c'e' un alone continuo: senza quello si vedrebbero solo le
// strisce, e fra una e l'altra il fondo resterebbe spento.
// 'fringe' (0 = nessuno) e' la tinta del contorno sfumato attorno a ogni raggio:
// si ricava dalla differenza fra l'onda morbida e quella appuntita, cioe' e'
// esattamente la spalla del raggio. Va steso PRIMA del colore pieno, che poi
// ne ricopre il centro lasciandolo visibile solo ai bordi.
void draw_rising_rays(int x, int y, int w, int h,
                      int count, int strength, uint32_t color, uint32_t fringe) {
    if (w < 2 || h < 2) return;
    if (count <= 0) {
        count = 9 + w / 16;
        if (count > 20) count = 20;
    }

    int cx = x + w / 2;
    int sy = y + h;                       // sorgente: il bordo piu' basso

    // Il ventaglio si chiude PRIMA dei fianchi e della cima del riquadro:
    // e' cosi' che l'arco resta intero invece di essere tagliato di netto.
    int hw = (w / 2) * 80 / 100; if (hw < 1) hw = 1;
    int reach = h * 105 / 100;

    int base = strength * 38 / 100;       // alone continuo sotto ai raggi

    for (int py = y; py < y + h; py++) {
        int up = sy - py;
        if (up <= 0) continue;

        int ny = up * 1024 / reach;
        if (ny > 1400) continue;

        for (int px = x; px < x + w; px++) {
            int nx = (px - cx) * 1024 / hw;
            int anx = nx < 0 ? -nx : nx;

            // Distanza radiale approssimata (ottagonale): niente radice, e
            // l'attenuazione diventa un'ellisse invece di un rettangolo.
            int mxv = anx > ny ? anx : ny;
            int mnv = anx > ny ? ny : anx;
            int rd = mxv + mnv * 3 / 8;
            if (rd >= 1024) continue;

            // Fra lineare e quadratica: la quadratica pura spegneva il
            // bagliore troppo presto, ben prima di arrivare al testo.
            int lin = 255 - rd * 255 / 1024;
            int fall = (lin * lin / 255 + lin) / 2;

            int a = iatan2(ny, nx);
            int p = (a * count) & 4095;
            int tri = (p < 2048) ? p : 4096 - p;

            int w1 = tri * 255 / 2048;      // onda morbida: tutto il raggio
            int w2 = w1 * w1 / 255;         // onda appuntita: il solo nucleo

            // Vicino alla sorgente gli angoli si affollano: senza questa
            // dissolvenza il vertice diventa rumore invece che un fuoco.
            if (rd < 150) { w1 = w1 * rd / 150; w2 = w2 * rd / 150; }

            // Contorno: la spalla del raggio, cioe' cio' che resta togliendo
            // il nucleo dall'onda morbida.
            if (fringe) {
                int sh = (w1 - w2) * 4;
                if (sh > 255) sh = 255;
                int af = sh * fall / 255;
                if (af > 0) blend_pixel(px, py, fringe, af);
            }

            int alpha = (base + strength * w2 / 255) * fall / 255;
            if (alpha > 255) alpha = 255;
            if (alpha > 0) blend_pixel(px, py, color, alpha);
        }
    }

}

// ============================================================================
// PANNELLO IN VETRO CREPATO, ILLUMINATO DAL BASSO
// ============================================================================
// Le crepe nascono da DUE tassellazioni di Voronoi sovrapposte: una a maglia
// larga per le fratture principali e una fine per la crettatura. E' cio' che
// distingue il vetro rotto da una texture: grandi schegge percorse da crepe
// sottili, non un reticolo uniforme.
//
// Ogni frattura ha un nucleo acceso E un solco scuro appena a lato: e' il
// contrasto fra i due che la fa leggere come uno spacco nello spessore,
// invece che come una riga disegnata sopra.
//
// Tutto e' calcolato UNA VOLTA in coordinate schermo e tenuto in cache.

#define GLASS_MAX_H  48
#define GLASS_COARSE 15      // maglia delle schegge grandi
#define GLASS_FINE    7      // maglia della crettatura

static uint8_t glass_lit[GLASS_MAX_H][1024];    // nucleo luminoso della crepa
static uint8_t glass_dark[GLASS_MAX_H][1024];   // solco scuro a lato
static int glass_ready = 0;

static int gfx_isqrt(int n) {
    if (n <= 0) return 0;
    int x = n, y = (x + 1) / 2;
    while (y < x) { x = y; y = (x + n / x) / 2; }
    return x;
}

static unsigned glass_hash(int a, int b) {
    unsigned h = (unsigned)a * 374761393u + (unsigned)b * 668265263u;
    h = (h ^ (h >> 13)) * 1274126177u;
    return h ^ (h >> 16);
}

// Distanza dal confine fra due celle: e' li' che passa la frattura, perche'
// il confine di Voronoi e' la bisettrice fra due centri, quindi una retta.
static int glass_edge(int x, int y, int cell) {
    int cx = x / cell, cy = y / cell;
    int b1 = 0x7FFFFFFF, b2 = 0x7FFFFFFF;

    for (int oy = -1; oy <= 1; oy++) {
        for (int ox = -1; ox <= 1; ox++) {
            unsigned hs = glass_hash(cx + ox, (cy + oy) * 32 + cell);
            int fx = (cx + ox) * cell + (int)(hs % (unsigned)cell);
            int fy = (cy + oy) * cell + (int)((hs >> 8) % (unsigned)cell);
            int dx = fx - x, dy = fy - y;
            int d2 = dx * dx + dy * dy;
            if (d2 < b1)      { b2 = b1; b1 = d2; }
            else if (d2 < b2) { b2 = d2; }
        }
    }
    return gfx_isqrt(b2) - gfx_isqrt(b1);
}

void glass_build(void) {
    for (int y = 0; y < GLASS_MAX_H; y++) {
        for (int x = 0; x < 1024; x++) {
            int ec = glass_edge(x, y, GLASS_COARSE);
            int ef = glass_edge(x, y, GLASS_FINE);

            int lc = 255 - ec * 255 / 3;          // frattura principale
            if (lc < 0) lc = 0;
            int lf = 255 - ef * 255 / 2;          // crettatura fine
            if (lf < 0) lf = 0;
            lf = lf * 55 / 100;                   // molto piu' tenue

            glass_lit[y][x] = (uint8_t)(lc > lf ? lc : lf);

            // Il solco comincia dove finisce il nucleo, e solo sulle
            // fratture principali: sulla crettatura sarebbe sporcizia.
            int dk = 0;
            if (ec >= 3 && ec < 7) dk = (7 - ec) * 70 / 4;
            glass_dark[y][x] = (uint8_t)dk;
        }
    }
    glass_ready = 1;
}

void draw_glass_panel(int x, int y, int w, int h, uint32_t glow) {
    if (!glass_ready) glass_build();
    if (h < 3 || w < 3) return;

    

    int span = h * 62 / 100;        // quanto in alto arriva la luce
    if (span < 1) span = 1;

    int x0 = x,     x1 = x + w;
    if (x0 < 0) x0 = 0;
    if (x1 > fb_width) x1 = fb_width;

    for (int py = y; py < y + h; py++) {
        if (py < 0 || py >= fb_height) continue;

        int up = (y + h - 1) - py;              // 0 = riga piu' bassa
        int gf = 255 - up * 255 / span;         // intensita' della luce
        if (gf < 0) gf = 0;
        gf = gf * gf / 255;                     // caduta morbida

        

        // Il vetro e' piu' latteo in alto, dove riflette invece di trasmettere
        int frost = 46 + up * 26 / h;

        

        for (int px = x0; px < x1; px++) {
            blend_pixel(px, py, 0xFFFFFFFF, frost);

            // Luce che sale dal basso
            if (gf > 0) blend_pixel(px, py, glow, gf * 165 / 255);

            // Nucleo acceso sul bordo inferiore (la striscia LED)
            if (up < 2) blend_pixel(px, py, glow, 120);

        
            // Filo di luce appena dentro il bordo nero, sui tre lati a
            //    vista. E' lo spessore della lastra che prende luce.
            if (py == y + 1 || px == x + 1 || px == x + w - 2)
                blend_pixel(px, py, 0xFFFFFFFF, 110);
        }
    }

    // Bordo nero sottile su alto, sinistra e destra. In basso non serve:
    // li' il pannello e' tagliato dal bordo dello schermo.
    draw_rect(x, y, w, 1, 0xFF000000);
    draw_rect(x, y, 1, h, 0xFF000000);
    draw_rect(x + w - 1, y, 1, h, 0xFF000000);
}



// ============================================================================
// DISPLAY DI VETRO
// Una lastra di vetro colorato incassata nella finestra: il corpo e' piu' scuro
// in alto e si schiarisce verso il basso, come illuminato da sotto; un'ombra
// interna sotto il bordo superiore la fa sembrare incassata; un riflesso lucido
// copre la meta' alta, con la classica linea d'orizzonte netta del vetro Aqua.
// ============================================================================

// Scala i tre canali di un colore: f = 255 lo lascia com'e', 0 lo annerisce
static uint32_t gd_scale(uint32_t c, int f) {
    if (f < 0) f = 0;
    if (f > 255) f = 255;
    uint32_t r = ((c >> 16) & 0xFF) * (uint32_t)f / 255;
    uint32_t g = ((c >> 8)  & 0xFF) * (uint32_t)f / 255;
    uint32_t b = (c & 0xFF)         * (uint32_t)f / 255;
    return 0xFF000000 | (r << 16) | (g << 8) | b;
}

// Mescola due colori: a = 0 tutto c1, 255 tutto c2
static uint32_t gd_mix(uint32_t c1, uint32_t c2, int a) {
    if (a <= 0) return c1;
    if (a >= 255) return c2;
    int ia = 255 - a;
    uint32_t r = (((c1 >> 16) & 0xFF) * ia + ((c2 >> 16) & 0xFF) * a) / 255;
    uint32_t g = (((c1 >> 8)  & 0xFF) * ia + ((c2 >> 8)  & 0xFF) * a) / 255;
    uint32_t b = ((c1 & 0xFF) * ia + (c2 & 0xFF) * a) / 255;
    return 0xFF000000 | (r << 16) | (g << 8) | b;
}

void draw_glass_display(int x, int y, int w, int h, uint32_t tint) {
    if (w < 8 || h < 8) return;

    int r = h / 4;
    if (r > 8) r = 8;
    if (r < 3) r = 3;

    uint32_t rim  = gd_scale(tint, 110);       // bordo, piu' scuro del vetro
    int band_h    = h * 9 / 20;                // il riflesso copre il 45% alto
    int gloss_r   = (r > 3) ? r - 2 : 1;

    for (int py = y; py < y + h; py++) {
        if (py < 0 || py >= fb_height) continue;
        int dy = py - y;

        for (int px = x; px < x + w; px++) {
            if (px < 0 || px >= fb_width) continue;

            int cov = rrect_coverage(px, py, x, y, w, h, r);
            if (cov <= 0) continue;

            // Corpo: scuro in alto, piu' chiaro in basso (luce da sotto)
            uint32_t c = gd_scale(tint, 150 + 105 * dy / (h - 1));

            // Ombra interna sotto il bordo superiore: il vetro e' incassato
            if (dy < 5) c = gd_scale(c, 255 - (5 - dy) * 18);

            // Bagliore sottile sul fondo
            if (dy >= h - 4) c = gd_mix(c, 0xFFFFFFFF, (dy - (h - 5)) * 14);

            // Riflesso lucido: piu' intenso in cima, poi si spegne, e finisce
            // con un orlo netto che da' l'effetto della lastra curva
            int g = rrect_coverage(px, py, x + 3, y + 2, w - 6, band_h, gloss_r);
            if (g > 0) {
                int a = 95 - 75 * (dy - 2) / band_h;
                if (a < 0) a = 0;
                c = gd_mix(c, 0xFFFFFFFF, a * g / 255);
            }

            // Bordo: l'anello esterno di un pixel, antialiasato
            int inner = rrect_coverage(px, py, x + 1, y + 1, w - 2, h - 2, r - 1);
            c = gd_mix(rim, c, inner);

            blend_pixel(px, py, c, cov);
        }
    }
}

// Il punto del font e' una barretta di un pixel che scende una riga sotto la
// base delle cifre: a grandezza normale non si nota, ma ingrandito sembra una
// virgola. Qui usiamo un punto quadrato appoggiato sulla base delle cifre.
static const uint8_t gd_dot[16] = { [11] = 0xC0, [12] = 0xC0 };

// Testo proporzionale in grassetto ingrandito: ogni pixel del glifo diventa un
// blocco di scale x scale, largo un pixel in piu' per l'effetto grassetto.
// Con scale = 1 coincide con draw_prop_string_bold. alpha < 255 lo rende
// trasparente, per esempio per disegnarne l'ombra.
void draw_prop_string_bold_scaled(const char* s, int x, int y, uint32_t color, int scale, int alpha) {
    if (scale < 1) scale = 1;
    int cx = x;
    for (; *s; s++) {
        unsigned char uc = (unsigned char)*s;
        if (uc == ' ') { cx += 5 * scale + 1; continue; }
        if (!FONT_HAS(uc)) { cx += 8 * scale + 1; continue; }

        int wdt = font_prop[uc].width;
        const uint8_t* glyph = (uc == '.') ? gd_dot : font_prop[uc].glyph;
        for (int row = 0; row < 16; row++) {
            uint8_t bits = glyph[row];
            if (!bits) continue;
            for (int col = 0; col < wdt; col++) {
                if (!(bits & (1 << (7 - col)))) continue;
                int bx = cx + col * scale, by = y + row * scale;
                for (int yy = 0; yy < scale; yy++)
                    for (int xx = 0; xx <= scale; xx++)
                        blend_pixel(bx + xx, by + yy, color, alpha);
            }
        }
        cx += (wdt + 1) * scale + 1;
    }
}

int get_prop_string_width_bold_scaled(const char* s, int scale) {
    if (scale < 1) scale = 1;
    int w = 0;
    for (; *s; s++) {
        unsigned char uc = (unsigned char)*s;
        if (uc == ' ')      w += 5 * scale + 1;
        else if (!FONT_HAS(uc)) w += 8 * scale + 1;
        else                w += (font_prop[uc].width + 1) * scale + 1;
    }
    return w;
}