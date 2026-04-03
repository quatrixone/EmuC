#include "nes.h"
#include "tables.h"

static void clock_envelope(struct NES *nes) {
    for (int ch = 0; ch < 2; ch++) {
        if (nes->pulse[ch].env_start) {
            nes->pulse[ch].env_start = 0;
            nes->pulse[ch].env_vol = 15;
            nes->pulse[ch].env_counter = nes->pulse[ch].vol_period;
        } else if (nes->pulse[ch].env_counter > 0) {
            nes->pulse[ch].env_counter--;
        } else {
            nes->pulse[ch].env_counter = nes->pulse[ch].vol_period;
            if (nes->pulse[ch].env_vol > 0) nes->pulse[ch].env_vol--;
            else if (nes->pulse[ch].halt) nes->pulse[ch].env_vol = 15;
        }
    }
    if (nes->noise.env_start) {
        nes->noise.env_start = 0;
        nes->noise.env_vol = 15;
        nes->noise.env_counter = nes->noise.vol_period;
    } else if (nes->noise.env_counter > 0) {
        nes->noise.env_counter--;
    } else {
        nes->noise.env_counter = nes->noise.vol_period;
        if (nes->noise.env_vol > 0) nes->noise.env_vol--;
        else if (nes->noise.halt) nes->noise.env_vol = 15;
    }
    if (nes->tri.linear_reload)
        nes->tri.linear_counter = nes->tri.linear_load;
    else if (nes->tri.linear_counter > 0)
        nes->tri.linear_counter--;
    if (!nes->tri.control)
        nes->tri.linear_reload = 0;
}

static void clock_length_sweep(struct NES *nes) {
    for (int ch = 0; ch < 2; ch++) {
        if (!nes->pulse[ch].halt && nes->pulse[ch].length > 0)
            nes->pulse[ch].length--;
    }
    if (!nes->tri.control && nes->tri.length > 0)
        nes->tri.length--;
    if (!nes->noise.halt && nes->noise.length > 0)
        nes->noise.length--;

    for (int ch = 0; ch < 2; ch++) {
        u16 delta = nes->pulse[ch].timer >> nes->pulse[ch].sweep_shift;
        u16 target;
        if (nes->pulse[ch].sweep_neg) {
            target = nes->pulse[ch].timer - delta;
            if (ch == 0) target--;
        } else {
            target = nes->pulse[ch].timer + delta;
        }
        int muted = (nes->pulse[ch].timer < 8 || target > 0x7FF);

        if (nes->pulse[ch].sweep_reload) {
            u8 old = nes->pulse[ch].sweep_counter;
            nes->pulse[ch].sweep_counter = nes->pulse[ch].sweep_period;
            if (nes->pulse[ch].sweep_en && old == 0 && !muted)
                nes->pulse[ch].timer = target;
            nes->pulse[ch].sweep_reload = 0;
        } else if (nes->pulse[ch].sweep_counter > 0) {
            nes->pulse[ch].sweep_counter--;
        } else {
            nes->pulse[ch].sweep_counter = nes->pulse[ch].sweep_period;
            if (nes->pulse[ch].sweep_en && !muted)
                nes->pulse[ch].timer = target;
        }
    }
}

void apu_write_reg(struct NES *nes, u16 addr, u8 val) {
    int ch;
    switch (addr) {
    case 0x4000: case 0x4004:
        ch = (addr >> 2) & 1;
        nes->pulse[ch].duty = (val >> 6) & 3;
        nes->pulse[ch].halt = (val >> 5) & 1;
        nes->pulse[ch].const_vol = (val >> 4) & 1;
        nes->pulse[ch].vol_period = val & 0xF;
        break;
    case 0x4001: case 0x4005:
        ch = (addr >> 2) & 1;
        nes->pulse[ch].sweep_en = (val >> 7) & 1;
        nes->pulse[ch].sweep_period = (val >> 4) & 7;
        nes->pulse[ch].sweep_neg = (val >> 3) & 1;
        nes->pulse[ch].sweep_shift = val & 7;
        nes->pulse[ch].sweep_reload = 1;
        break;
    case 0x4002: case 0x4006:
        ch = (addr >> 2) & 1;
        nes->pulse[ch].timer = (nes->pulse[ch].timer & 0x700) | val;
        break;
    case 0x4003: case 0x4007:
        ch = (addr >> 2) & 1;
        nes->pulse[ch].timer = (nes->pulse[ch].timer & 0xFF) | ((val & 7) << 8);
        if (nes->pulse[ch].enabled)
            nes->pulse[ch].length = length_table[(val >> 3) & 0x1F];
        nes->pulse[ch].env_start = 1;
        nes->pulse[ch].duty_pos = 0;
        break;
    case 0x4008:
        nes->tri.control = (val >> 7) & 1;
        nes->tri.linear_load = val & 0x7F;
        break;
    case 0x400A:
        nes->tri.timer = (nes->tri.timer & 0x700) | val;
        break;
    case 0x400B:
        nes->tri.timer = (nes->tri.timer & 0xFF) | ((val & 7) << 8);
        if (nes->tri.enabled)
            nes->tri.length = length_table[(val >> 3) & 0x1F];
        nes->tri.linear_reload = 1;
        break;
    case 0x400C:
        nes->noise.halt = (val >> 5) & 1;
        nes->noise.const_vol = (val >> 4) & 1;
        nes->noise.vol_period = val & 0xF;
        break;
    case 0x400E:
        nes->noise.mode = (val >> 7) & 1;
        nes->noise.period_idx = val & 0xF;
        break;
    case 0x400F:
        if (nes->noise.enabled)
            nes->noise.length = length_table[(val >> 3) & 0x1F];
        nes->noise.env_start = 1;
        break;
    case 0x4015:
        nes->pulse[0].enabled = val & 1;
        nes->pulse[1].enabled = (val >> 1) & 1;
        nes->tri.enabled = (val >> 2) & 1;
        nes->noise.enabled = (val >> 3) & 1;
        if (!nes->pulse[0].enabled) nes->pulse[0].length = 0;
        if (!nes->pulse[1].enabled) nes->pulse[1].length = 0;
        if (!nes->tri.enabled) nes->tri.length = 0;
        if (!nes->noise.enabled) nes->noise.length = 0;
        break;
    case 0x4017:
        nes->frame_mode = (val >> 7) & 1;
        nes->frame_irq_inhibit = (val >> 6) & 1;
        nes->frame_counter = 0;
        if (nes->frame_mode) {
            clock_envelope(nes);
            clock_length_sweep(nes);
        }
        break;
    }
}

void apu_step(struct NES *nes, int cycles) {
    for (int c = 0; c < cycles; c++) {
        if ((nes->total_cycles + c) & 1) {
            for (int ch = 0; ch < 2; ch++) {
                if (nes->pulse[ch].timer_count > 0) nes->pulse[ch].timer_count--;
                else {
                    nes->pulse[ch].timer_count = nes->pulse[ch].timer;
                    nes->pulse[ch].duty_pos = (nes->pulse[ch].duty_pos + 1) & 7;
                }
            }
        }

        if (nes->tri.timer_count > 0) nes->tri.timer_count--;
        else {
            nes->tri.timer_count = nes->tri.timer;
            if (nes->tri.length > 0 && nes->tri.linear_counter > 0)
                nes->tri.step = (nes->tri.step + 1) & 31;
        }

        if (nes->noise.timer_count > 0) nes->noise.timer_count--;
        else {
            nes->noise.timer_count = nes->is_pal
                ? noise_period_pal[nes->noise.period_idx]
                : noise_period_ntsc[nes->noise.period_idx];
            int fb = nes->noise.mode
                ? ((nes->noise.shift_reg & 1) ^ ((nes->noise.shift_reg >> 6) & 1))
                : ((nes->noise.shift_reg & 1) ^ ((nes->noise.shift_reg >> 1) & 1));
            nes->noise.shift_reg = (nes->noise.shift_reg >> 1) | (fb << 14);
        }

        nes->frame_counter++;
        int *steps = nes->fc_step[nes->frame_mode];
        if      (nes->frame_counter == steps[0]) clock_envelope(nes);
        else if (nes->frame_counter == steps[1]) { clock_envelope(nes); clock_length_sweep(nes); }
        else if (nes->frame_counter == steps[2]) clock_envelope(nes);
        else if (nes->frame_counter == steps[4]) { clock_envelope(nes); clock_length_sweep(nes); }
        else if (nes->frame_counter >= steps[5]) nes->frame_counter = 0;

        nes->sample_acc += SAMPLE_RATE;
        if (nes->sample_acc >= nes->cpu_freq) {
            nes->sample_acc -= nes->cpu_freq;

            int p1 = 0, p2 = 0, tri_out = 0, noi = 0;
            for (int ch = 0; ch < 2; ch++) {
                if (!nes->pulse[ch].length || nes->pulse[ch].timer < 8) continue;
                u16 delta = nes->pulse[ch].timer >> nes->pulse[ch].sweep_shift;
                u16 target = nes->pulse[ch].sweep_neg
                    ? (nes->pulse[ch].timer - delta - (ch == 0 ? 1 : 0))
                    : (nes->pulse[ch].timer + delta);
                if (target > 0x7FF) continue;
                if (!duty_wave[nes->pulse[ch].duty][nes->pulse[ch].duty_pos]) continue;
                u8 vol = nes->pulse[ch].const_vol ? nes->pulse[ch].vol_period : nes->pulse[ch].env_vol;
                if (ch == 0) p1 = vol; else p2 = vol;
            }
            if (nes->tri.length > 0 && nes->tri.linear_counter > 0 && nes->tri.timer >= 2)
                tri_out = tri_wave[nes->tri.step];
            if (nes->noise.length > 0 && !(nes->noise.shift_reg & 1))
                noi = nes->noise.const_vol ? nes->noise.vol_period : nes->noise.env_vol;

            int ps = p1 + p2; if (ps > 30) ps = 30;
            int ts = 3 * tri_out + 2 * noi; if (ts > 202) ts = 202;
            s32 raw = (s32)(pulse_mix[ps] + tnd_mix[ts]);

            s32 lp = (raw * 6 + nes->lpf_prev * 10) / 16;
            nes->lpf_prev = (s16)lp;
            s32 hp = lp - nes->hpf_in + (nes->hpf_out * 255 / 256);
            nes->hpf_in = lp;
            nes->hpf_out = hp;

            s16 sample = (s16)(hp > 32767 ? 32767 : (hp < -32768 ? -32768 : hp));
            if (nes->audio_pos < 2048) {
                nes->audio_buf[nes->audio_pos * 2] = sample;
                nes->audio_buf[nes->audio_pos * 2 + 1] = sample;
                nes->audio_pos++;
            }
        }
    }
}

void apu_flush(struct NES *nes) {
    if (nes->audio_handle < 0 || !nes->audio_out_fn) return;
    while (nes->audio_pos >= SAMPLES_PER_BUF) {
        NC(nes->gadget, nes->audio_out_fn,
           (u64)nes->audio_handle, (u64)nes->audio_buf, 0, 0, 0, 0);
        int rem = nes->audio_pos - SAMPLES_PER_BUF;
        for (int i = 0; i < rem * 2; i++)
            nes->audio_buf[i] = nes->audio_buf[SAMPLES_PER_BUF * 2 + i];
        nes->audio_pos = rem;
    }
}
