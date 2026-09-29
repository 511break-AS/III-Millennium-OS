#ifndef GFX_H
#define GFX_H

#include <stdint.h>
#include <stddef.h>

// --- Tipi condivisi ---
typedef struct { uint8_t width; uint8_t glyph[16]; } prop_char_t;
typedef struct {
    uint32_t desktop_bg;

    // Colori dominanti (logo, scrollbar, menu hover, banner)
    uint32_t primary_color;
    uint32_t secondary_color;

    // Taskbar e bottoni
    uint32_t taskbar_bg;
    uint32_t taskbar_btn_active;
    uint32_t taskbar_btn_idle;

    // Finestre
    uint32_t window_bg;
    uint32_t window_close_btn;
    uint32_t window_ctrl_btn;
    uint32_t titlebar_base;
    uint32_t titlebar_lines;
    uint32_t menu_hover_bg;
    uint32_t menu_hover_lines;

    // Elementi UI
    uint32_t tooltip_bg;
    uint32_t scrollbar_track;

    // Bottoni delle applicazioni (vetro Aqua)
    uint32_t button_body;          // corpo "a riposo"
    uint32_t button_body_pressed;  // corpo "premuto" (colore voluto, non compensato)
    uint32_t button_veil;          // tinta verso cui sfuma il velo
    uint32_t button_rim;           // bordo saturo
    uint32_t button_rays;          // tinta dei raggi
    uint32_t taskbar_glow;         // luce che illumina il vetro dal basso

} theme_palette_t;

// --- Costanti ---
#define CURSOR_W 12
#define CURSOR_H 16
#define TILE_W   64
#define TILE_H   16
#define PANEL_BAR_H    24
#define PANEL_SHADOW_H 8

// --- Stato condiviso ---
extern uint32_t* fb;
extern int fb_width, fb_height;
extern uint32_t backbuffer[];
extern uint32_t* shadow_buffer;
extern int shadow_ready;
extern int cursor_drawn_x, cursor_drawn_y;
extern theme_palette_t active_theme;
extern prop_char_t font_prop[256];
// Il carattere ha un disegno nel font? Tutto l'ASCII, e le lettere accentate
// Latin-1 che ne hanno uno; gli altri byte oltre il 127 restano "sconosciuti".
#define FONT_HAS(uc) ((uc) < 128 || font_prop[(unsigned char)(uc)].width > 0)

// --- Primitive ---
void draw_pixel(int x, int y, uint32_t color);
void draw_rect(int x, int y, int w, int h, uint32_t color);
void swap_buffers(void);
void blend_pixel(int x, int y, uint32_t color, int alpha);
int  rrect_coverage(int px, int py, int x, int y, int w, int h, int rad);
int  circle_coverage(int px, int py, int cx, int cy, int rad);
int  smooth255(int s);
void fill_circle_aa(int cx, int cy, int rad, uint32_t color);

// --- Cursore ---
void cursor_restore(void);
void cursor_draw_at(int mx, int my);
void cursor_move(int mx, int my);

// --- Widget ---
void draw_button_shadow(int x, int y, int w, int h, int rad, int pressed);
void draw_button(int x, int y, int w, int h, uint32_t bg, uint32_t light, uint32_t dark);
void draw_textfield(int x, int y, int w, int h, uint32_t bg);
void draw_window(int x, int y, int w, int h, int is_active);
void draw_glass_panel(int x, int y, int w, int h, uint32_t glow);
void draw_glass_display(int x, int y, int w, int h, uint32_t tint);
void draw_os_logo(int x, int y);
uint32_t gfx_derive_rim(uint32_t body);
uint32_t gfx_derive_veil(uint32_t body);
uint32_t gfx_derive_pressed(uint32_t body);
#define BUTTON_DEFAULT 0   // l'app chiede il colore del tema
// --- Manopole di draw_button (globali: impostare prima, ripristinare dopo) ---
extern int gfx_veil_bot, gfx_veil_feather, gfx_button_opacity, gfx_button_radius;
extern uint32_t gfx_veil_tint, gfx_rim_tint;
extern int gfx_clip_y1;
void gfx_style_aqua(uint32_t veil, uint32_t rim);
void gfx_style_reset(void);

#define RAYS_AUTO 0
void draw_button_rays(int x, int y, int w, int h, int count, int strength, uint32_t color);
void draw_rising_rays(int x, int y, int w, int h, int count, int strength, uint32_t color, uint32_t fringe);

// --- Testo ---
void draw_char(char c, int x, int y, uint32_t color, int scale);
void draw_string(const char *s, int x, int y, uint32_t color, int scale);
int  get_prop_string_width(const char* s);
int  get_prop_string_width_bold(const char* s);
void draw_prop_string(const char *s, int x, int y, uint32_t color);
void draw_prop_string_bold(const char *s, int x, int y, uint32_t color);
void draw_prop_string_bold_shadow(const char *s, int x, int y, int dy, uint32_t color, int alpha);
void draw_prop_string_clipped(const char *s, int x, int y, uint32_t color, int cx, int cy, int cw, int ch);
void draw_prop_string_bold_scaled(const char* s, int x, int y, uint32_t color, int scale, int alpha);
int  get_prop_string_width_bold_scaled(const char* s, int scale);

// --- Icone ---
void draw_icon_16(int icon_id, int x, int y);
void draw_ui_glyph_16(const uint16_t* glyph, int x, int y, uint32_t color);

extern const uint16_t ui_glyph_close[16];
extern const uint16_t ui_glyph_restore[16];
extern const uint16_t ui_glyph_maximize[16];
extern const uint16_t ui_glyph_minimize[16];


#endif