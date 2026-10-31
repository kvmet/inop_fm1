/* SPDX-License-Identifier: GPL-3.0-only */
/* INOP: one compilation unit. Each file uses what the files above it define, so the order matters. */
#include "fm1_cc.h"

#define FELUCCA_FLASH 1
#define FELUCCA_OTA 1
#define FELUCCA_OTA_DRYRUN 0
#define FELUCCA_CDC 0
#define FELUCCA_UAC 0
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

#define FM1_LED_DIM_NS 6400u     /* every unlit LED's glow: about 2/30 of lit. Longer than the 595 shift, so each scan tick waits 2 to 3 us */

#include <stdint.h>
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
#include "fm_engine.c"
#include "perc_engine.c"
#include "swarm_engine.c"
#define POOL_SECTION __attribute__((section(".pool")))
#include "fx_sends.c"
#include "phys_engine.c"
#include "voice_steal.c"
#include "voices.c"
#include "usb.c"
#include "audio_out.c"

#include "fm1_flash.h"
#include "storage_hw.c"
#include "ota.c"
#include "ota_hw.c"

#include "ui.c"
#include "app.c"
