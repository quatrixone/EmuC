#include "nes.h"
#include "tables.h"

static void inc_scroll_y(struct NES *nes) {
    u16 v = nes->vram_addr;
    if ((v & 0x7000) != 0x7000) { v += 0x1000; }
    else {
        v &= ~0x7000;
        int cy = (v & 0x03E0) >> 5;
        if (cy == 29) { cy = 0; v ^= 0x0800; }
        else if (cy == 31) cy = 0;
        else cy++;
        v = (v & ~0x03E0) | (cy << 5);
    }
    nes->vram_addr = v;
}

static void copy_scroll_x(struct NES *nes) {
    nes->vram_addr = (nes->vram_addr & 0xFBE0) | (nes->temp_addr & 0x041F);
}

static void copy_scroll_y(struct NES *nes) {
    nes->vram_addr = (nes->vram_addr & 0x041F) | (nes->temp_addr & 0xFBE0);
}

void render_scanline(struct NES *nes, int y) {
    u8 *line = &nes->screen[y * NES_W];
    u8 bg_opaque[NES_W];
    for (int x = 0; x < NES_W; x++) { line[x] = nes->palette[0]; bg_opaque[x] = 0; }

    if (nes->ppu_mask & 0x08) {
        u16 v = nes->vram_addr;
        u16 pat = (nes->ppu_ctrl & 0x10) ? 0x1000 : 0;

        for (int tile = 0; tile < 33; tile++) {
            int cx = v & 0x1F;
            int cy = (v >> 5) & 0x1F;
            int fy = (v >> 12) & 7;
            u16 nt = 0x2000 | (v & 0x0C00);

            u8 idx = ppu_read(nes, nt | (cy << 5) | cx);
            u8 lo = ppu_read(nes, pat + (u16)idx * 16 + fy);
            u8 hi = ppu_read(nes, pat + (u16)idx * 16 + fy + 8);

            u8 at = ppu_read(nes, nt | 0x03C0 | ((cy >> 2) << 3) | (cx >> 2));
            u8 pal_idx = (at >> (((cy & 2) << 1) | (cx & 2))) & 3;

            for (int px = 0; px < 8; px++) {
                int sx = tile * 8 + px - nes->fine_x;
                if (sx < 0 || sx >= NES_W) continue;
                u8 color = ((hi >> (7-px)) & 1) << 1 | ((lo >> (7-px)) & 1);
                if (color) {
                    line[sx] = nes->palette[pal_idx * 4 + color];
                    bg_opaque[sx] = 1;
                }
            }

            if ((v & 0x1F) == 31) { v &= ~0x1F; v ^= 0x0400; }
            else v++;
        }
    }

    if (nes->ppu_mask & 0x10) {
        int sph = (nes->ppu_ctrl & 0x20) ? 16 : 8;
        u16 spr_pat = (nes->ppu_ctrl & 0x08) ? 0x1000 : 0;
        int cnt = 0;
        u8 sprites[8];
        int has_sp0 = 0;

        for (int i = 0; i < 64; i++) {
            int oy = nes->oam[i*4];
            if (oy >= 0xEF) continue;
            int sy = oy + 1;
            if (y < sy || y >= sy + sph) continue;
            if (cnt < 8) {
                sprites[cnt] = i;
                if (i == 0) has_sp0 = 1;
                cnt++;
            } else {
                nes->ppu_status |= 0x20;
                break;
            }
        }

        int spr_clip = !(nes->ppu_mask & 0x04);
        int sp0_left_clip = !(nes->ppu_mask & 0x02) || spr_clip;

        for (int s = cnt - 1; s >= 0; s--) {
            int i = sprites[s];
            int sy = nes->oam[i*4] + 1;
            int tile = nes->oam[i*4+1];
            int attr = nes->oam[i*4+2];
            int sx = nes->oam[i*4+3];

            int row = y - sy;
            if (attr & 0x80) row = sph - 1 - row;

            u16 pa;
            if (sph == 16) {
                u16 bk = (tile & 1) ? 0x1000 : 0;
                u8 t = tile & 0xFE;
                if (row >= 8) { t++; row -= 8; }
                pa = bk + t * 16 + row;
            } else {
                pa = spr_pat + tile * 16 + row;
            }

            u8 lo = ppu_read(nes, pa);
            u8 hi = ppu_read(nes, pa + 8);
            u8 spal = (attr & 3) + 4;

            for (int px = 0; px < 8; px++) {
                int bx = (attr & 0x40) ? px : (7 - px);
                u8 c = ((hi >> bx) & 1) << 1 | ((lo >> bx) & 1);
                if (!c) continue;
                int dx = sx + px;
                if (dx >= NES_W) continue;
                if (has_sp0 && i == 0 && bg_opaque[dx] && dx < 255
                    && (nes->ppu_mask & 0x08)
                    && !(sp0_left_clip && dx < 8))
                    nes->ppu_status |= 0x40;
                if (spr_clip && dx < 8) continue;
                if ((attr & 0x20) && bg_opaque[dx]) continue;
                line[dx] = nes->palette[spal * 4 + c];
            }
        }
    }

    if (!(nes->ppu_mask & 0x02))
        for (int x = 0; x < 8; x++) line[x] = nes->palette[0];
}

void run_frame(struct NES *nes) {
    nes->cycles = 0;
    nes->in_vblank = 0;
    int target = 0, sl_acc = 0;
    int sl_num = nes->is_pal ? (341 * 5) : 341;
    int sl_den = nes->is_pal ? 16 : 3;
    int total_sl = nes->num_scanlines;

    for (int y = 0; y < 240; y++) {
        if (nes->ppu_mask & 0x18) copy_scroll_x(nes);

        render_scanline(nes, y);
        if (nes->ppu_mask & 0x18) inc_scroll_y(nes);

        if (nes->mapper == 4 && (nes->ppu_mask & 0x18)) {
            if (nes->mmc3_irq_count == 0 || nes->mmc3_irq_reload) {
                nes->mmc3_irq_count = nes->mmc3_irq_latch;
                nes->mmc3_irq_reload = 0;
            } else {
                nes->mmc3_irq_count--;
            }
            if (nes->mmc3_irq_count == 0 && nes->mmc3_irq_enable)
                nes->irq_pending = 1;
        }

        sl_acc += sl_num;
        target += sl_acc / sl_den;
        sl_acc %= sl_den;

        int before = nes->cycles;
        while (nes->cycles < target) cpu_step(nes);
        int ran = nes->cycles - before;
        apu_step(nes, ran);
        nes->total_cycles += ran;
    }

    for (int y = 240; y < total_sl; y++) {
        if (y == 241) { nes->ppu_status |= 0x80; nes->in_vblank = 1; }
        if (y == total_sl - 1) {
            nes->ppu_status &= ~0xE0;
            nes->in_vblank = 0;
            if (nes->ppu_mask & 0x18) copy_scroll_y(nes);
        }
        sl_acc += sl_num;
        target += sl_acc / sl_den;
        sl_acc %= sl_den;

        int before = nes->cycles;
        if (y == 241) {
            cpu_step(nes);
            if (nes->ppu_ctrl & 0x80) nes->nmi_pending = 1;
        }
        while (nes->cycles < target) cpu_step(nes);
        int ran = nes->cycles - before;
        apu_step(nes, ran);
        nes->total_cycles += ran;
    }
}

void scale_to_framebuf(u32 *fb, const u8 *scr, u8 mask) {
    int grey = mask & 0x01;
    int emph_r = (mask >> 5) & 1;
    int emph_g = (mask >> 6) & 1;
    int emph_b = (mask >> 7) & 1;

    for (int ny = 0; ny < NES_H; ny++) {
        int sy = OFF_Y + ny * SCALE;
        for (int nx = 0; nx < NES_W; nx++) {
            u8 idx = scr[ny * NES_W + nx] & 0x3F;
            if (grey) idx &= 0x30;
            u32 c = nes_palette[idx];
            if (emph_r | emph_g | emph_b) {
                u32 r = (c >> 16) & 0xFF;
                u32 g = (c >> 8) & 0xFF;
                u32 b = c & 0xFF;
                if (emph_g | emph_b) r = r * 3 / 4;
                if (emph_r | emph_b) g = g * 3 / 4;
                if (emph_r | emph_g) b = b * 3 / 4;
                c = 0xFF000000 | (r << 16) | (g << 8) | b;
            }
            int sx = OFF_X + nx * SCALE;
            for (int dy = 0; dy < SCALE; dy++) {
                u32 *row = &fb[(sy + dy) * SCR_W + sx];
                for (int dx = 0; dx < SCALE; dx++)
                    row[dx] = c;
            }
        }
    }
}

int str_len(const char *s) {
    int n = 0;
    while (*s++) n++;
    return n;
}

void draw_char(u8 *scr, int x, int y, char ch, u8 color) {
    int idx = 0;
    if (ch >= 'a' && ch <= 'z') ch -= 32;
    if (ch >= 32 && ch <= 90) idx = ch - 32;
    const u8 *glyph = font_data[idx];

    for (int r = 0; r < 8; r++) {
        u8 bits = glyph[r];
        for (int c = 0; c < 8; c++) {
            if (bits & (0x80 >> c)) {
                int px = x + c, py = y + r;
                if (px >= 0 && px < NES_W && py >= 0 && py < NES_H)
                    scr[py * NES_W + px] = color;
            }
        }
    }
}

void draw_str(u8 *scr, int x, int y, const char *s, u8 color) {
    while (*s) {
        draw_char(scr, x, y, *s, color);
        x += 8;
        s++;
    }
}

void draw_centered(u8 *scr, int y, const char *s, u8 color) {
    int x = (NES_W - str_len(s) * 8) / 2;
    if (x < 0) x = 0;
    draw_str(scr, x, y, s, color);
}

void draw_hline(u8 *scr, int y, int x1, int x2, u8 color) {
    if (y < 0 || y >= NES_H) return;
    for (int x = x1; x < x2 && x < NES_W; x++)
        scr[y * NES_W + x] = color;
}

int is_rom_file(const char *name) {
    int len = str_len(name);
    if (len < 5) return 0;
    char a = name[len-4], b = name[len-3], c = name[len-2], d = name[len-1];
    if (a != '.') return 0;
    if (b >= 'A' && b <= 'Z') b += 32;
    if (c >= 'A' && c <= 'Z') c += 32;
    if (d >= 'A' && d <= 'Z') d += 32;
    return (b == 'r' && c == 'o' && d == 'm') || (b == 'n' && c == 'e' && d == 's');
}

void extract_rom_name(const char *fn, char *out, int max) {
    int i = 0;
    while (fn[i] && fn[i] != '.' && i < max - 1) {
        out[i] = fn[i];
        i++;
    }
    out[i] = '\0';
}

