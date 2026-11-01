/* SPDX-License-Identifier: GPL-3.0-only */
/* INOP: one compilation unit. Each file uses what the files above it define, so the order matters. */
#include "fm1_cc.h"

#define FELUCCA_FLASH 1
#define FELUCCA_OTA 1
#define FELUCCA_OTA_DRYRUN 0
#ifndef INOP_PROBE
#define INOP_PROBE 0
#endif
#define FELUCCA_CDC INOP_PROBE
#define FELUCCA_UAC INOP_PROBE
#define FELUCCA_UAC_48K 0
#define FELUCCA_UART 0
#define FELUCCA_CPU2 0
#define FM1_CPU1_USED 0
#ifndef FELUCCA_ID
#define FELUCCA_ID "FM-1_700"
#endif
#ifndef FELUCCA_VERSION
#define FELUCCA_VERSION "inop-dev"
#endif

/* audio code run from RAM, out of the XIP cache */
#define AUDIO_RAM __attribute__((section(".ram_hot"), flatten))
/* RAM and XIP are too far apart for a direct call: a call between them goes through a pointer the compiler cannot fold */
static inline __attribute__((always_inline)) void *audio_far(void *p) { void *volatile q = p; return q; }
#define AUDIO_FAR(fn) ((__typeof__(&fn))audio_far((void *)&fn))

#define FM1_LED_DIM_NS 6400u     /* every unlit LED's glow: about 2/30 of lit. Longer than the 595 shift, so each scan tick waits 2 to 3 us */

#include <stdint.h>
#if INOP_PROBE
static volatile uint32_t probe_buttons, probe_notes;
#define FM1_INPUT_INJECT(id) ((id) < 14u ? probe_buttons >> (id) & 1u : probe_notes >> ((id) - 14u) & 1u)
static void probe_tap_fill(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint16_t c);
static void probe_tap_blit(uint32_t x, uint32_t y, uint32_t w, uint32_t h, const uint16_t *px);
#define LCD_TAP_FILL probe_tap_fill
#define LCD_TAP_BLIT probe_tap_blit
#endif
#include "fm1_time.h"
#include "fm1_sys.h"
#include "fm1_irq.h"
#include "fm1_guard.h"
#include "fm1_input.h"
#include "fm1_timer.h"
#include "fm1_audio.h"
#include "fm1_adc.h"
#include "fm1_lcd_hw.h"
#include "felucca_tables.h"

#include "libc.c"
#include "lcd.c"
#include "gfx.c"

#include "model.h"
#define BOOTGUARD_MAGIC 0x42475244u
struct { uint32_t magic, failed, pending; } bootguard __attribute__((section(".noinit")));   /* two failed boots: UBOOT */
#include "model.c"
#include "seq.c"
#include "overdub.c"
#include "voice_chain.c"
#include "master.c"
#include "fm_engine.c"
#include "perc_engine.c"
#include "swarm_engine.c"
#include "felucca_wave_tables.h"
#include "wave_engine.c"
#define POOL_SECTION __attribute__((section(".pool")))
#include "fx_sends.c"
#include "voice_steal.c"
#include "voices.c"
#include "usb.c"
#include "audio_out.c"

#include "fm1_flash.h"
#include "storage_hw.c"
#include "ota.c"
#include "ota_hw.c"

#include "ui.c"
#if INOP_PROBE
#include "probe.c"
#endif
#include "app.c"
