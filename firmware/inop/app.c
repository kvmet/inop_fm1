/* SPDX-License-Identifier: GPL-3.0-only */
/* Boot and main loop. */
extern uint32_t _data_start[], _data_end[], _data_load[], _bss_start[], _bss_end[];
extern uint32_t _pool_start[], _pool_end[], _rt_start[], _rt_end[], _rt_load[], _ramx_start[], _ramx_end[];


/* the scan outranks the audio ISR so it keeps its pace during a render; USB waits for the next tick after it */
void fm1_timer5_irq(void)
{
    static uint32_t sub, owed, last, acc;
    uint32_t t0 = fm1_ticks(), usb_due = sub % 5u == 0u;
    fm1_timer5_ack();
    fm1_input_tick();
    acc += t0 - last;
    last = t0;
    while (acc >= 1000u * FM1_TICKS_PER_US) {
        acc -= 1000u * FM1_TICKS_PER_US;
        fm1_ms++;
    }
    if (usb_due)
        owed = 1;
    if (++sub == 10u)
        sub = 0;
    if (audio_in_isr) {
#if FELUCCA_UAC
        if (usb_due)
            uac_service();               /* a packet a millisecond, or the recording breaks */
#endif
        t5_nested_ticks += fm1_ticks() - t0;
        return;
    }
    if (owed)
        usb_poll();
    owed = 0;
}
extern void isr_timer5(void);

static void hexs(char *b, uint32_t v)
{
    uint32_t i;
    for (i = 0; i < 8u; i++)
        b[i] = "0123456789ABCDEF"[(v >> (28u - 4u * i)) & 15u];
    b[8] = 0;
}

static void fm1_fault(const fm1_crash_t *c)
{
    char b[12];
    uint32_t t0;
    fm1_audio_stop();
    lcd_fill(0, 0, 240, 240, UI_CRASH_BG);
    draw_text_line(0, 8, 240, &AF_M, "INOP CRASH", UI_CRASH_INK, UI_CRASH_BG, 1);
    hexs(b, c->vec);
    draw_text_line(10, 40, 220, &AF_M, b, UI_CRASH_INK, UI_CRASH_BG, 0);
    hexs(b, c->pc);
    draw_text_line(10, 60, 220, &AF_M, b, UI_CRASH_INK, UI_CRASH_BG, 0);
    t0 = fm1_ticks();
    while ((uint32_t)(fm1_ticks() - t0) < 4000u * 1000u * FM1_TICKS_PER_US)
        ;
    fm1_reboot();
}

static void enter_uboot(const char *msg)
{
    fm1_audio_stop();
    lcd_fill(0, 0, 240, 240, T_BG);
    draw_text_box(0, 110, 240, &AF_M, msg, T_THEME, 1);
    usb_detach();
    fm1_delay_ms(30);
    bootguard.pending = 0;
    fm1_enter_uboot();
}

static void inop_main(void)
{
    uint32_t irq = irq_save(), uboot_t0 = 0, frame = 0;
    flash_ok = FL_FAR(fl_jedec_ram)() == 0x856014u;
    irq_restore(irq);
    if (flash_ok) {
        fl_plain_window_init();
        ota_boot_cleanup();
    }
    palette_set(0);
    lcd_init();
    lcd_fill(0, 0, 240, 240, T_BG);
    fm1_input_init();
    fm1_adc_init();
    ui_init();
    voices_measure();
    audio_init();
    usb_start();
    fm1_timer5_start(isr_timer5, 4);
    fm1_guard_lock_top();
    fm1_irq_enable_all();

    for (;;) {
        uint32_t m = fm1_ms, both = 1u << B_OCTDN | 1u << B_OCTUP;
        int32_t a = fm1_adc_read(FM1_ADC_MASTER);
        fm1_wdt_feed();
        usb_retry(fm1_ms);
        if (fm1_ms > 30000u && bootguard.pending) {
            bootguard.pending = 0;
            bootguard.failed = 0;
        }
        if (a >= 0)
            master_q12 = ((uint32_t)a * (uint32_t)a) >> 8;
        if ((fm1_in.buttons & both) != both)
            uboot_t0 = fm1_ms;
        else if (fm1_ms - uboot_t0 > 5000u)
            enter_uboot("UPDATE MODE");
        ota_service();
        if (usb.ota_req) {
            usb.ota_req = 0;
            if (flash_ok)
                ota_session();
            lcd_fill(0, 0, 240, 240, T_BG);
            grid_invalidate();
        }
        if (usb.uboot_req)
            enter_uboot("UPDATE MODE (USB)");
#if INOP_PROBE
        probe_service();
#endif
        ui_input();
        ui_leds();
        if (++frame % 4u == 0u)
            ui_draw();
        while (fm1_ms - m < 15u)
            ui_step_service();
    }
}

void fm1_cstart(void)
{
    uint32_t *s, *d, p3, src, wdt;
    fm1_time_init();
    fm1_reset_reason();
    p3 = fm1_boot.p3_rst;
    src = fm1_boot.rst_src;
    wdt = fm1_boot.wdt_con;
    fm1_wdt_arm(0x0D);
    if (bootguard.magic != BOOTGUARD_MAGIC) {
        bootguard.magic = BOOTGUARD_MAGIC;
        bootguard.failed = 0;
        bootguard.pending = 0;
    }
    if (bootguard.pending)
        bootguard.failed++;
    bootguard.pending = 1;
    if (bootguard.failed >= 2u) {
        bootguard.failed = 0;
        bootguard.pending = 0;
        fm1_enter_uboot();
    }
    fm1_irq_init();
    for (d = _bss_start; d < _bss_end; d++)
        *d = 0;
    for (d = _pool_start; d < _pool_end; d++)
        *d = 0;
    for (d = _ramx_start; d < _ramx_end; d++)
        *d = 0;
    for (s = _data_load, d = _data_start; d < _data_end; s++, d++)
        *d = *s;
    for (s = _rt_load, d = _rt_start; d < _rt_end; s++, d++)
        *d = *s;                                /* flash driver code that must run from RAM */
    fm1_mailbox_clear();
    fm1_guard_enable(FM1_GUARD_STACK | FM1_GUARD_WRITE | FM1_GUARD_BUS | FM1_GUARD_PC);
    fm1_boot.p3_rst = (uint8_t)p3;
    fm1_boot.rst_src = src;
    fm1_boot.wdt_con = (uint8_t)wdt;
    inop_main();
    for (;;)
        ;
}
