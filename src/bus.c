#include "nes.h"
#include "tables.h"

static u16 mirror_nt(struct NES *nes, u16 addr) {
    addr &= 0x0FFF;
    switch (nes->mirror) {
    case 0: return ((addr / 0x400) & ~1) * 0x200 + (addr & 0x3FF);
    case 1: return addr & 0x7FF;
    case 2: return addr & 0x3FF;
    case 3: return (addr & 0x3FF) + 0x400;
    }
    return addr & 0x7FF;
}

static void vram_step(struct NES *nes) {
    if ((nes->ppu_mask & 0x18) && !nes->in_vblank) {
        if ((nes->vram_addr & 0x1F) == 31) {
            nes->vram_addr &= ~0x1F;
            nes->vram_addr ^= 0x0400;
        } else nes->vram_addr++;
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
    } else {
        nes->vram_addr += (nes->ppu_ctrl & 4) ? 32 : 1;
    }
}

u8 ppu_read(struct NES *nes, u16 addr) {
    addr &= 0x3FFF;

    if (addr < 0x2000) {
        if (nes->chr_is_ram)
            return nes->chr[addr & 0x1FFF];

        switch (nes->mapper) {
        case 1: {
            int bank;
            if (nes->mmc1_ctrl & 0x10) {
                bank = (addr < 0x1000) ? nes->mmc1_chr0 : nes->mmc1_chr1;
                return nes->chr[(bank * 0x1000 + (addr & 0xFFF)) % nes->chr_size];
            }
            bank = nes->mmc1_chr0 >> 1;
            return nes->chr[(bank * 0x2000 + addr) % nes->chr_size];
        }
        case 3: case 87: case 185:
            return nes->chr[(nes->chr_bank * 0x2000 + addr) % nes->chr_size];
        case 4: case 206: {
            int bank;
            if (nes->mmc3_chr_mode == 0) {
                if      (addr<0x0400) bank = nes->mmc3_regs[0] & 0xFE;
                else if (addr<0x0800) bank = nes->mmc3_regs[0] | 1;
                else if (addr<0x0C00) bank = nes->mmc3_regs[1] & 0xFE;
                else if (addr<0x1000) bank = nes->mmc3_regs[1] | 1;
                else if (addr<0x1400) bank = nes->mmc3_regs[2];
                else if (addr<0x1800) bank = nes->mmc3_regs[3];
                else if (addr<0x1C00) bank = nes->mmc3_regs[4];
                else                  bank = nes->mmc3_regs[5];
            } else {
                if      (addr<0x0400) bank = nes->mmc3_regs[2];
                else if (addr<0x0800) bank = nes->mmc3_regs[3];
                else if (addr<0x0C00) bank = nes->mmc3_regs[4];
                else if (addr<0x1000) bank = nes->mmc3_regs[5];
                else if (addr<0x1400) bank = nes->mmc3_regs[0] & 0xFE;
                else if (addr<0x1800) bank = nes->mmc3_regs[0] | 1;
                else if (addr<0x1C00) bank = nes->mmc3_regs[1] & 0xFE;
                else                  bank = nes->mmc3_regs[1] | 1;
            }
            return nes->chr[(bank * 0x400 + (addr & 0x3FF)) % nes->chr_size];
        }
        case 9: case 10: {
            int bank = (addr < 0x1000)
                ? nes->mmc2_chr_lo[nes->mmc2_latch0]
                : nes->mmc2_chr_hi[nes->mmc2_latch1];
            u8 val = nes->chr[(bank * 0x1000 + (addr & 0xFFF)) % nes->chr_size];
            if (addr == 0x0FD8) nes->mmc2_latch0 = 0;
            else if (addr == 0x0FE8) nes->mmc2_latch0 = 1;
            else if (addr >= 0x1FD8 && addr <= 0x1FDF) nes->mmc2_latch1 = 0;
            else if (addr >= 0x1FE8 && addr <= 0x1FEF) nes->mmc2_latch1 = 1;
            return val;
        }
        case 66:
            return nes->chr[(nes->chr_bank * 0x2000 + addr) % nes->chr_size];
        case 69: {
            int bank = nes->fme7_chr[(addr >> 10) & 7];
            return nes->chr[(bank * 0x400 + (addr & 0x3FF)) % nes->chr_size];
        }
        default:
            return nes->chr[addr % nes->chr_size];
        }
    }

    if (addr < 0x3F00)
        return nes->vram[mirror_nt(nes, addr - 0x2000)];

    u8 idx = addr & 0x1F;
    if ((idx & 0x13) == 0x10) idx &= 0x0F;
    return nes->palette[idx];
}

void ppu_write(struct NES *nes, u16 addr, u8 val) {
    addr &= 0x3FFF;
    if (addr < 0x2000) {
        if (nes->chr_is_ram) nes->chr[addr & 0x1FFF] = val;
    } else if (addr < 0x3F00) {
        nes->vram[mirror_nt(nes, addr - 0x2000)] = val;
    } else {
        u8 idx = addr & 0x1F;
        if ((idx & 0x13) == 0x10) idx &= 0x0F;
        nes->palette[idx] = val;
    }
}

u8 cpu_read(struct NES *nes, u16 addr) {
    if (addr < 0x2000) return nes->ram[addr & 0x7FF];

    if (addr < 0x4000) {
        switch (addr & 7) {
        case 2: {
            u8 r = (nes->ppu_status & 0xE0) | (nes->read_buf & 0x1F);
            nes->ppu_status &= ~0x80;
            nes->write_toggle = 0;
            return r;
        }
        case 4: return nes->oam[nes->oam_addr];
        case 7: {
            u8 r = nes->read_buf;
            u16 a = nes->vram_addr & 0x3FFF;
            nes->read_buf = ppu_read(nes, a);
            if (a >= 0x3F00) r = nes->read_buf;
            vram_step(nes);
            return r;
        }
        default: return 0;
        }
    }

    if (addr >= 0x6000 && addr < 0x8000)
        return nes->sram[addr - 0x6000];

    if (addr == 0x4015) {
        u8 r = 0;
        if (nes->pulse[0].length > 0) r |= 1;
        if (nes->pulse[1].length > 0) r |= 2;
        if (nes->tri.length > 0) r |= 4;
        if (nes->noise.length > 0) r |= 8;
        return r;
    }

    if (addr == 0x4016) {
        u8 r = (nes->pad_shift & 1);
        nes->pad_shift >>= 1;
        return r | 0x40;
    }
    if (addr == 0x4017) return 0x40;

    if (addr >= 0x8000) {
        u32 off = addr - 0x8000;
        switch (nes->mapper) {
        case 1: {
            int mode = (nes->mmc1_ctrl >> 2) & 3;
            int bank = nes->mmc1_prg & 0x0F;
            if (mode <= 1)
                return nes->prg[((bank >> 1) * 0x8000 + off) % nes->prg_size];
            if (mode == 2) {
                if (addr < 0xC000) return nes->prg[off];
                return nes->prg[(bank * 0x4000 + (addr - 0xC000)) % nes->prg_size];
            }
            if (addr < 0xC000)
                return nes->prg[(bank * 0x4000 + off) % nes->prg_size];
            return nes->prg[(nes->prg_banks - 1) * 0x4000 + (addr - 0xC000)];
        }
        case 2: case 94:
            if (addr < 0xC000) return nes->prg[(nes->prg_bank * 0x4000 + off) % nes->prg_size];
            return nes->prg[(nes->prg_banks - 1) * 0x4000 + (addr - 0xC000)];
        case 4: {
            int b;
            if (nes->mmc3_prg_mode == 0) {
                if      (addr<0xA000) b = nes->mmc3_regs[6];
                else if (addr<0xC000) b = nes->mmc3_regs[7];
                else if (addr<0xE000) b = nes->prg_banks*2 - 2;
                else                  b = nes->prg_banks*2 - 1;
            } else {
                if      (addr<0xA000) b = nes->prg_banks*2 - 2;
                else if (addr<0xC000) b = nes->mmc3_regs[7];
                else if (addr<0xE000) b = nes->mmc3_regs[6];
                else                  b = nes->prg_banks*2 - 1;
            }
            return nes->prg[(b * 0x2000 + (addr & 0x1FFF)) % nes->prg_size];
        }
        case 7: case 34: case 66:
            return nes->prg[(nes->prg_bank * 0x8000 + off) % nes->prg_size];
        case 9:
            if (addr < 0xA000) return nes->prg[(nes->prg_bank * 0x2000 + off) % nes->prg_size];
            return nes->prg[(nes->prg_size - 0x6000 + (addr - 0xA000)) % nes->prg_size];
        case 10:
            if (addr < 0xC000) return nes->prg[(nes->prg_bank * 0x4000 + off) % nes->prg_size];
            return nes->prg[(nes->prg_banks - 1) * 0x4000 + (addr - 0xC000)];
        case 69: {
            int slot = (addr >> 13) & 3;
            return nes->prg[(nes->fme7_prg[slot] * 0x2000 + (addr & 0x1FFF)) % nes->prg_size];
        }
        case 180:
            if (addr < 0xC000) return nes->prg[off];
            return nes->prg[(nes->prg_bank * 0x4000 + (addr - 0xC000)) % nes->prg_size];
        case 206: {
            int b;
            if      (addr<0xA000) b = nes->mmc3_regs[6] & 0x0F;
            else if (addr<0xC000) b = nes->mmc3_regs[7] & 0x0F;
            else if (addr<0xE000) b = nes->prg_banks*2 - 2;
            else                  b = nes->prg_banks*2 - 1;
            return nes->prg[(b * 0x2000 + (addr & 0x1FFF)) % nes->prg_size];
        }
        default:
            if (nes->prg_size == 0x4000) off &= 0x3FFF;
            return nes->prg[off];
        }
    }
    return 0;
}

void cpu_write(struct NES *nes, u16 addr, u8 val) {
    if (addr < 0x2000) { nes->ram[addr & 0x7FF] = val; return; }

    if (addr < 0x4000) {
        switch (addr & 7) {
        case 0: {
            u8 prev = nes->ppu_ctrl;
            nes->ppu_ctrl = val;
            nes->temp_addr = (nes->temp_addr & 0xF3FF) | ((val & 3) << 10);
            if (!(prev & 0x80) && (val & 0x80) && (nes->ppu_status & 0x80))
                nes->nmi_pending = 1;
            break;
        }
        case 1: nes->ppu_mask = val; break;
        case 3: nes->oam_addr = val; break;
        case 4: nes->oam[nes->oam_addr++] = val; break;
        case 5:
            if (!nes->write_toggle) {
                nes->fine_x = val & 7;
                nes->temp_addr = (nes->temp_addr & 0xFFE0) | (val >> 3);
            } else {
                nes->temp_addr = (nes->temp_addr & 0x0C1F) | ((val & 7) << 12) | ((val >> 3) << 5);
            }
            nes->write_toggle ^= 1;
            break;
        case 6:
            if (!nes->write_toggle)
                nes->temp_addr = (nes->temp_addr & 0x00FF) | ((val & 0x3F) << 8);
            else {
                nes->temp_addr = (nes->temp_addr & 0xFF00) | val;
                nes->vram_addr = nes->temp_addr;
            }
            nes->write_toggle ^= 1;
            break;
        case 7:
            ppu_write(nes, nes->vram_addr, val);
            vram_step(nes);
            break;
        }
        return;
    }

    if (addr == 0x4014) {
        u16 base = (u16)val << 8;
        for (int i = 0; i < 256; i++)
            nes->oam[(nes->oam_addr + i) & 0xFF] = cpu_read(nes, base + i);
        nes->cycles += 513 + (nes->cycles & 1);
        return;
    }

    if ((addr >= 0x4000 && addr <= 0x4013) || addr == 0x4015 || addr == 0x4017) {
        apu_write_reg(nes, addr, val);
        return;
    }

    if (addr == 0x4016) {
        if (nes->pad_strobe && !(val & 1))
            nes->pad_shift = nes->pad_state;
        nes->pad_strobe = val & 1;
        return;
    }

    if (addr >= 0x6000 && addr < 0x8000) {
        nes->sram[addr - 0x6000] = val;
        return;
    }

    if (addr >= 0x8000) {
        switch (nes->mapper) {
        case 1:
            if (val & 0x80) {
                nes->mmc1_shift = 0; nes->mmc1_count = 0;
                nes->mmc1_ctrl |= 0x0C;
            } else {
                nes->mmc1_shift |= ((val & 1) << nes->mmc1_count);
                if (++nes->mmc1_count == 5) {
                    int reg = (addr >> 13) & 3;
                    switch (reg) {
                    case 0: nes->mmc1_ctrl = nes->mmc1_shift;
                        switch (nes->mmc1_ctrl & 3) {
                        case 0: nes->mirror = 2; break;
                        case 1: nes->mirror = 3; break;
                        case 2: nes->mirror = 1; break;
                        case 3: nes->mirror = 0; break;
                        } break;
                    case 1: nes->mmc1_chr0 = nes->mmc1_shift; break;
                    case 2: nes->mmc1_chr1 = nes->mmc1_shift; break;
                    case 3: nes->mmc1_prg = nes->mmc1_shift; break;
                    }
                    nes->mmc1_shift = 0; nes->mmc1_count = 0;
                }
            }
            break;
        case 2: nes->prg_bank = val & (nes->prg_banks - 1); break;
        case 3: nes->chr_bank = val & 3; break;
        case 4:
            if (addr < 0xA000) {
                if (addr & 1) nes->mmc3_regs[nes->mmc3_select & 7] = val;
                else { nes->mmc3_select = val; nes->mmc3_prg_mode = (val >> 6) & 1; nes->mmc3_chr_mode = (val >> 7) & 1; }
            } else if (addr < 0xC000) {
                if (!(addr & 1)) nes->mirror = (val & 1) ? 0 : 1;
            } else if (addr < 0xE000) {
                if (addr & 1) { nes->mmc3_irq_count = 0; nes->mmc3_irq_reload = 1; }
                else nes->mmc3_irq_latch = val;
            } else {
                nes->mmc3_irq_enable = (addr & 1) ? 1 : 0;
            }
            break;
        case 7: nes->prg_bank = val & 7; nes->mirror = (val & 0x10) ? 3 : 2; break;
        case 9: case 10:
            if      (addr < 0xB000) nes->prg_bank = val & 0x0F;
            else if (addr < 0xC000) nes->mmc2_chr_lo[0] = val & 0x1F;
            else if (addr < 0xD000) nes->mmc2_chr_lo[1] = val & 0x1F;
            else if (addr < 0xE000) nes->mmc2_chr_hi[0] = val & 0x1F;
            else if (addr < 0xF000) nes->mmc2_chr_hi[1] = val & 0x1F;
            else nes->mirror = (val & 1) ? 0 : 1;
            break;
        case 34: nes->prg_bank = val; break;
        case 66: nes->prg_bank = (val >> 4) & 3; nes->chr_bank = val & 3; break;
        case 69:
            if (addr < 0xA000) { nes->fme7_cmd = val & 0x0F; }
            else if (addr < 0xC000) {
                u8 cmd = nes->fme7_cmd;
                if (cmd < 8) nes->fme7_chr[cmd] = val;
                else if (cmd < 12) nes->fme7_prg[cmd - 8] = val & 0x3F;
                else if (cmd == 12) {
                    switch (val & 3) {
                    case 0: nes->mirror = 1; break; case 1: nes->mirror = 0; break;
                    case 2: nes->mirror = 2; break; case 3: nes->mirror = 3; break;
                    }
                }
            }
            break;
        case 87: nes->chr_bank = ((val >> 1) & 1) | ((val << 1) & 2); break;
        case 94: nes->prg_bank = (val >> 2) & 7; break;
        case 180: nes->prg_bank = val & 7; break;
        case 185: nes->chr_bank = val & 3; break;
        case 206:
            if (addr < 0xA000) {
                if (addr & 1) nes->mmc3_regs[nes->mmc3_select & 7] = val;
                else nes->mmc3_select = val & 7;
            }
            break;
        }
    }
}