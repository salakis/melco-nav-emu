/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 melco-nav-emu contributors */

/* The second bank is the capture path (wavedev.dll 00ff1648). */
#define CUP28_PCM_RATE 44100
#define CUP28_PCM_FRAME_BYTES 4
#define CUP28_VOICE_PREROLL (CUP28_PCM_RATE * CUP28_PCM_FRAME_BYTES / 5)
#define CUP28_VOICE_RESERVE (CUP28_PCM_RATE * CUP28_PCM_FRAME_BYTES / 2)
#define CUP28_AUDIO_DMA_MAX 4096
#define CUP28_KIWI_BLOCK 16
#define CUP28_KIWI_SAMPLES 30

/* VPD 010927e8 reverses each four-byte word on the bus. STUB: select
 * the hardware voice decoder by its framing while codec routing is
 * unmodelled. The native Western-disc guidance is 22,050 Hz mono.
 * See datashenanigans.pl/2022/10/kiwi-gps-voice001-me-sound-file-specification/. */
static bool cup28_audio_is_kiwi(uint32_t address, size_t size)
{
    uint8_t probe[8 * CUP28_KIWI_BLOCK];
    bool nonzero = false;

    if (size < sizeof(probe) || address < 0x0c000000 ||
        address > 0x10000000 - sizeof(probe) ||
        address_space_read(&address_space_memory, address,
                           MEMTXATTRS_UNSPECIFIED, probe,
                           sizeof(probe)) != MEMTX_OK) {
        return false;
    }
    for (unsigned i = 3; i < sizeof(probe); i += CUP28_KIWI_BLOCK) {
        unsigned header = probe[i];
        if ((header & 0x0c) || (header >> 4) > 12) {
            return false;
        }
        nonzero |= header != 0;
    }
    return nonzero;
}

static void cup28_audio_decode_kiwi(Cup28MachineState *s,
                                  const uint8_t *input, size_t size,
                                  uint8_t *output)
{
    static const int coef[4][2] = { { 0, 0 }, { 60, 0 },
                                   { 115, 52 }, { 98, 55 } };

    for (size_t block = 0; block + CUP28_KIWI_BLOCK <= size;
         block += CUP28_KIWI_BLOCK) {
        unsigned header = input[block + 3];
        unsigned scale = 1u << (12 - MIN(header >> 4, 12));
        const int *predictor = coef[header & 3];
        for (unsigned i = 0; i < CUP28_KIWI_SAMPLES; i++) {
            unsigned byte = input[block + ((1 + i / 2) ^ 3)];
            int nibble = (byte >> ((i & 1) ? 0 : 4)) & 15;
            int sample = (nibble - ((nibble & 8) ? 16 : 0)) * (int)scale;

            sample += s->p28_audio_history[0] * predictor[0] / 64;
            sample -= s->p28_audio_history[1] * predictor[1] / 64;
            s->p28_audio_history[1] = s->p28_audio_history[0];
            s->p28_audio_history[0] = sample;
            sample = MIN(MAX(sample, -32768), 32767);
            /* Repeat the mono sample in both channels and two frames. */
            for (unsigned j = 0; j < 4; j++) {
                stw_le_p(output, sample);
                output += 2;
            }
        }
    }
}

static void cup28_audio_output(void *opaque, int avail)
{
    Cup28MachineState *s = opaque;
    size_t queued = s->p28_audio_pcm_len - s->p28_audio_pcm_pos;
    bool voice_active = s->p28_audio_kiwi && (s->p28_audio_reg[0][4] & 1);
    size_t size;

    if (!queued) {
        /* An empty staging queue says nothing about samples already
         * accepted by CoreAudio. Re-prime only for a new stream. */
        if (!voice_active) {
            s->p28_audio_streaming = false;
        }
        return;
    }
    /* DMA completions follow the virtual clock. Give the host audio
     * clock a reserve before starting the voice.
     * Once DMA stops, drain short prompts and the final partial buffer. */
    if (voice_active && !s->p28_audio_streaming &&
        queued < CUP28_VOICE_PREROLL) {
        return;
    }
    size = MIN(queued, avail);
    s->p28_audio_pcm_pos += audio_be_write(
        s->p28_audio_backend, s->p28_audio_out,
        s->p28_audio_pcm + s->p28_audio_pcm_pos, size);
    s->p28_audio_streaming = true;
}

/* Arm the completion timer for the current chunk. */
static void cup28_audio_arm(Cup28MachineState *s)
{
    uint32_t *reg = s->p28_audio_reg[0];
    size_t bytes = s->p28_audio_dma_bytes;
    unsigned frames;

    if (s->p28_audio_kiwi) {
        frames = (bytes / CUP28_KIWI_BLOCK) * CUP28_KIWI_SAMPLES * 2;
        /* STUB: stage compressed transfers ahead of the wall-clock
         * sink while its software reserve is low. The output device
         * still plays the decoded samples at their original rate. */
        if (s->p28_audio_out &&
            s->p28_audio_pcm_len - s->p28_audio_pcm_pos < CUP28_VOICE_RESERVE) {
            frames /= 4;
        }
    } else {
        frames = bytes / CUP28_PCM_FRAME_BYTES;
    }
    reg[4] |= 1;
    timer_mod(s->p28_audio_timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
              (int64_t)MAX(frames, 1) * NANOSECONDS_PER_SECOND /
              CUP28_PCM_RATE);
}

static void cup28_audio_schedule(Cup28MachineState *s)
{
    uint32_t *reg = s->p28_audio_reg[0];
    unsigned slot = s->p28_audio_second_buffer ? 1 : 0;
    size_t total = (size_t)reg[slot + 2] * CUP28_PCM_FRAME_BYTES;

    if (!(reg[4] & 1)) {
        timer_del(s->p28_audio_timer);
    } else if (timer_pending(s->p28_audio_timer)) {
        return;
    } else if (total) {
        /* Buffers longer than one staging chunk play in CUP28_AUDIO_DMA_MAX
         * chunks; the completion interrupt fires only after the last one,
         * so no tail is silently dropped. */
        s->p28_audio_dma_address = reg[slot];
        s->p28_audio_dma_bytes = MIN(total, (size_t)CUP28_AUDIO_DMA_MAX);
        s->p28_audio_dma_remaining = total - s->p28_audio_dma_bytes;
        if (!slot) {
            s->p28_audio_kiwi = cup28_audio_is_kiwi(s->p28_audio_dma_address,
                                                  s->p28_audio_dma_bytes);
            memset(s->p28_audio_history, 0, sizeof(s->p28_audio_history));
        }
        cup28_audio_arm(s);
    } else {
        reg[4] &= ~1u;
    }
}

static void cup28_audio_complete(void *opaque)
{
    Cup28MachineState *s = opaque;
    uint32_t address = s->p28_audio_dma_address;
    size_t bytes = s->p28_audio_dma_bytes;
    uint8_t *pending = s->p28_dvd_control_reg + 0x30;
    uint8_t input[CUP28_AUDIO_DMA_MAX];

    if (bytes && address >= 0x0c000000 && address <= 0x10000000 - bytes &&
        address_space_read(&address_space_memory, address,
                           MEMTXATTRS_UNSPECIFIED, input,
                           bytes) == MEMTX_OK) {
        size_t output_bytes = s->p28_audio_kiwi
            ? bytes / CUP28_KIWI_BLOCK * CUP28_KIWI_SAMPLES * 8 : bytes;
        if (s->p28_audio_out) {
            size_t queued = s->p28_audio_pcm_len - s->p28_audio_pcm_pos;
            if (queued + output_bytes > sizeof(s->p28_audio_pcm)) {
                info_report("cup28: audio host queue overrun");
                queued = 0;
            }
            memmove(s->p28_audio_pcm, s->p28_audio_pcm + s->p28_audio_pcm_pos,
                    queued);
            s->p28_audio_pcm_pos = 0;
            s->p28_audio_pcm_len = queued + output_bytes;
            if (s->p28_audio_kiwi) {
                cup28_audio_decode_kiwi(s, input, bytes, s->p28_audio_pcm + queued);
            } else {
                memcpy(s->p28_audio_pcm + queued, input, bytes);
            }
        }
        s->p28_audio_frames += output_bytes / CUP28_PCM_FRAME_BYTES;
        trace_cup28_audio_dma_phys_bytes_frames(address, bytes, s->p28_audio_frames, s->p28_audio_kiwi ? "KIWI ADPCM" : "PCM");
    }

    if (s->p28_audio_dma_remaining) {
        /* Play the next chunk of an over-long buffer; the completion
         * interrupt below fires only once the whole buffer has played. */
        s->p28_audio_dma_address += bytes;
        s->p28_audio_dma_bytes = MIN(s->p28_audio_dma_remaining,
                                     (size_t)CUP28_AUDIO_DMA_MAX);
        s->p28_audio_dma_remaining -= s->p28_audio_dma_bytes;
        cup28_audio_arm(s);
        return;
    }

    /* NK 80084fe0 routes the CPLD bank at +30/+34. Its bit 1 handler
     * 80086280 clears that bit and reports the playback SYSINTR 0x12. */
    stl_le_p(pending, ldl_le_p(pending) | 2);
    cup28_dvd_irq(s);
    /* VPD 010926ec starts with bit 0. VPD 0109273c clears bit 1
     * to finish the in-flight buffer without another reload. */
    if (s->p28_audio_reg[0][4] & 2) {
        s->p28_audio_second_buffer = true;
        cup28_audio_schedule(s);
    } else {
        s->p28_audio_reg[0][4] &= ~1u;
        s->p28_audio_second_buffer = false;
    }
}

static uint64_t cup28_audio_read(void *opaque, hwaddr addr, unsigned size)
{
    Cup28MachineState *s = opaque;
    unsigned reg = (addr & 0xff) / 4;

    return reg < 5 ? s->p28_audio_reg[addr >> 8][reg] : 0;
}

static void cup28_audio_write(void *opaque, hwaddr addr, uint64_t value,
                              unsigned size)
{
    Cup28MachineState *s = opaque;
    unsigned bank = addr >> 8;
    unsigned reg = (addr & 0xff) / 4;

    if (reg < 5) {
        /* Both banks have addresses at +0/+4 and counts at +8/+c.
         * VPD 01092750/0109279c write the playback pairs in this order. */
        if (reg < 2) {
            value &= 0x1fffffff;
        }
        s->p28_audio_reg[bank][reg] = value;
        if (!bank && (reg == 4 || (reg == 3 && s->p28_audio_second_buffer))) {
            if (reg == 4 && !(value & 1)) {
                s->p28_audio_second_buffer = false;
            }
            cup28_audio_schedule(s);
        }
    }
}

static const MemoryRegionOps cup28_audio_ops = {
    .read = cup28_audio_read, .write = cup28_audio_write,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
    .endianness = DEVICE_LITTLE_ENDIAN,
};

static void cup28_audio_shutdown(Notifier *notifier, void *data)
{
    Cup28MachineState *s = container_of(notifier, Cup28MachineState,
                                        audio_shutdown);

    audio_be_close_out(s->p28_audio_backend, s->p28_audio_out);
    s->p28_audio_out = NULL;
}

void cup28_audio_init(Cup28MachineState *s, MemoryRegion *mem,
                             const char *audiodev)
{
    s->p28_audio_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL,
                                     cup28_audio_complete, s);
    if (audiodev) {
        const struct audsettings format = {
            .freq = CUP28_PCM_RATE, .nchannels = 2, .fmt = AUDIO_FORMAT_S16,
        };

        s->p28_audio_backend = audio_be_by_name(audiodev, &error_fatal);
        s->p28_audio_out = audio_be_open_out(s->p28_audio_backend, NULL,
                                            "CUP28 PCM", s,
                                            cup28_audio_output, &format);
        if (s->p28_audio_out) {
            audio_be_set_active_out(s->p28_audio_backend, s->p28_audio_out,
                                    true);
            s->audio_shutdown.notify = cup28_audio_shutdown;
            qemu_register_shutdown_notifier(&s->audio_shutdown);
        }
    }
    memory_region_init_io(&s->p28_audio_regs, OBJECT(s), &cup28_audio_ops, s,
                          "cup28-audio", 0x114);
    memory_region_add_subregion_overlap(mem, 0x07100100, &s->p28_audio_regs, 2);
}

