/*
 * ulp_pir_monitor.c — LP core PIR motion detector
 *
 * Runs on the ESP32-P4 LP RISC-V core while HP cores are in deep sleep.
 * Consumes ~1–5 mA total (LP core + RTC peripherals).
 *
 * Behaviour:
 *   • Configures a GPIO interrupt on pir_gpio_num (set by HP core before sleep)
 *   • On POSEDGE (PIR output goes high = motion detected):
 *       - Debounces: ignores edges within DEBOUNCE_MS of a recent wakeup
 *       - Calls ulp_lp_core_wakeup_main_processor() → HP cores boot
 *   • Loops indefinitely until HP cores shut it down on next sleep cycle
 *
 * Shared memory (RTC slow memory — survives deep sleep):
 *   ulp_pir_gpio_num  — GPIO number written by HP core before sleep entry
 *   ulp_wakeup_count  — monotonic count of wakeups (debug / telemetry)
 *   ulp_last_motion_ts— LP timer ticks at last wakeup trigger
 *
 * Build: compiled by ESP-IDF ULP toolchain as part of the main component.
 *   CMakeLists.txt invokes ulp_embed_binary() which compiles this file,
 *   links it, and produces a binary embedded in the app binary.
 */

#include "ulp_lp_core.h"
#include "ulp_lp_core_utils.h"
#include "ulp_lp_core_gpio.h"
#include "ulp_lp_core_interrupts.h"
#include "ulp_lp_core_lp_timer_shared.h"

#include <stdint.h>

/* Debounce window: ignore repeated edges within this many milliseconds.        */
#define DEBOUNCE_MS 500

/* LP timer runs at ~16 MHz on ESP32-P4; 16000 ticks ≈ 1 ms */
#define LP_TIMER_TICKS_PER_MS 16000ULL

/* ── Shared RTC memory variables ─────────────────────────────────────────────
 * These survive deep sleep and are accessible from both the LP core and,
 * after wakeup, from the HP core.                                              */
/* HP core accesses these as ulp_pir_gpio_num, ulp_wakeup_count, ulp_last_motion_ts
 * (the build system prepends "ulp_" to each exported symbol name).           */
volatile uint32_t pir_gpio_num   = 5;   /* default GPIO5; overwritten by HP */
volatile uint32_t wakeup_count   = 0;
volatile uint64_t last_motion_ts = 0;

/* ── LP core GPIO interrupt handler ─────────────────────────────────────────── */

void LP_CORE_ISR_ATTR ulp_lp_core_lp_io_intr_handler(void) {
    /* Clear interrupt status first to re-arm the interrupt */
    ulp_lp_core_gpio_clear_intr_status();

    /* Read current LP timer ticks */
    uint64_t now_ticks = ulp_lp_core_lp_timer_get_cycle_count();

    /* Debounce: suppress re-trigger within window */
    if (last_motion_ts != 0) {
        uint64_t elapsed_ms = (now_ticks - last_motion_ts) / LP_TIMER_TICKS_PER_MS;
        if (elapsed_ms < DEBOUNCE_MS) {
            return;
        }
    }

    /* Confirm GPIO is still high (not a noise spike) */
    if (ulp_lp_core_gpio_get_level((lp_io_num_t)pir_gpio_num) == 0) {
        return;
    }

    /* Record event */
    last_motion_ts = now_ticks;
    wakeup_count++;

    /* Wake HP cores — they will boot and restart the full pipeline */
    ulp_lp_core_wakeup_main_processor();
}

/* ── main ────────────────────────────────────────────────────────────────────
 * Entry point for the LP core program.
 * Configures the PIR GPIO interrupt, then waits for interrupts indefinitely.
 * Each GPIO POSEDGE fires ulp_lp_core_lp_io_intr_handler().               */

int main(void) {
    lp_io_num_t gpio = (lp_io_num_t)pir_gpio_num;

    /* Initialise the LP IO pin and configure as input */
    ulp_lp_core_gpio_init(gpio);
    ulp_lp_core_gpio_input_enable(gpio);
    ulp_lp_core_gpio_output_disable(gpio);

    /* Enable pull-down (PIR typically drives high on motion, floats low) */
    ulp_lp_core_gpio_pulldown_enable(gpio);
    ulp_lp_core_gpio_pullup_disable(gpio);

    /* Interrupt on rising edge — PIR output POSEDGE = motion */
    ulp_lp_core_gpio_intr_enable(gpio, GPIO_INTR_POSEDGE);

    /* Enable LP core interrupts globally */
    ulp_lp_core_intr_enable();

    /* Wait-for-interrupt loop — LP core wakes on GPIO interrupt, runs ISR,
     * returns here and waits again.  Lowest-power state available.         */
    while (1) {
        ulp_lp_core_wait_for_intr();
    }

    return 0; /* unreachable */
}
