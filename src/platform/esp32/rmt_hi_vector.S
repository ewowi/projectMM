/* rmt_hi_vector.S: the level-5 interrupt bridge for the classic-ESP32 RMT refill.
 *
 * Why this file exists, in one paragraph. On a classic ESP32 the RMT has no DMA: the peripheral
 * plays from a small on-chip block that an interrupt must refill every few dozen microseconds
 * for the whole frame, and a late refill re-clocks stale symbols into the strip (bits shift,
 * a red light shows blue). The IDF driver services that interrupt at level 1-3, and on Xtensa
 * every critical section raises PS.INTLEVEL to XCHAL_EXCM_LEVEL (3), so a spinlock anywhere on
 * the core holds the refill for its whole duration. Only a level-4/5 interrupt runs through a
 * critical section, and a level-4/5 handler cannot be written in C: it runs with the register
 * window in an undefined state and no stack, so this bridge saves the full register file,
 * switches to a private stack, calls one IRAM C function, and restores. It is derived from
 * Espressif's own hli_vectors.S (Apache-2.0, components/bt/controller/esp32), moved from level 4
 * to level 5 with the Bluetooth-specific parts removed. The C side is rmtHiIsr in
 * platform_esp32_rmt.cpp.
 *
 * The level-5 vector (interrupt 26) is flagged special in the IDF descriptor table, so
 * esp_intr_alloc refuses it for a peripheral source; the C side routes the RMT source to it
 * by hand (esp_rom_route_intr_matrix) and enables it on core 1. Nothing else uses level 5 on
 * this build: the interrupt watchdog and cache-error check sit on level 4
 * (CONFIG_ESP_SYSTEM_CHECK_INT_LEVEL_4), and Bluetooth is off. Both facts are asserted in C.
 */

#include "sdkconfig.h"

#if defined(__XTENSA__) && defined(CONFIG_IDF_TARGET_ESP32) && !defined(CONFIG_BTDM_CTRL_HLI)

#include <xtensa/coreasm.h>
#include <xtensa/corebits.h>
#include <xtensa/config/system.h>
#include "xtensa/config/core-isa.h"
#include "xtensa_context.h"
#include "soc/soc.h"

#define RMT_HI_LEVEL          5
#define RMT_HI_INTR_STACK     1024

/* Save area: 64 general registers plus 7 specials (WINDOWBASE, WINDOWSTART, SAR, LBEG, LEND,
 * LCOUNT, EPC1). Not the standard exception frame: laid out for the save/restore loops below. */
#define REG_FILE_SIZE         (64 * 4)
#define SPECREG_OFFSET        REG_FILE_SIZE
#define SPECREG_SIZE          (7 * 4)
#define REG_SAVE_AREA_SIZE    (SPECREG_OFFSET + SPECREG_SIZE)

#define sp a1

    .data
    /* 16-byte aligned, because the C handler's stack pointer is derived from the end of this
     * buffer and the Xtensa windowed ABI requires a 16-byte aligned SP. `.data` gives no such
     * guarantee on its own. The save area is aligned too: it is addressed with l32i/s32i, which
     * want 4, and 16 costs nothing here. */
    .align      16
_rmt_hi_intr_stack:
    .space      RMT_HI_INTR_STACK
    .align      16
_rmt_hi_save_ctx:
    .space      REG_SAVE_AREA_SIZE

    .section .iram1,"ax"
    .global     xt_highint5
    .type       xt_highint5,@function
    .align      4

xt_highint5:
    movi    a0, _rmt_hi_save_ctx
    /* save the four low registers; a0 arrives in EXCSAVE_5 */
    s32i    a1, a0, 4
    s32i    a2, a0, 8
    s32i    a3, a0, 12
    rsr     a2, XT_REG_EXCSAVE_5
    s32i    a2, a0, 0

    /* special registers */
    addi    a0, a0, SPECREG_OFFSET
    rsr     a2, XT_REG_WINDOWBASE
    s32i    a2, a0, 0
    rsr     a2, XT_REG_WINDOWSTART
    s32i    a2, a0, 4
    rsr     a2, XT_REG_SAR
    s32i    a2, a0, 8
    #if XCHAL_HAVE_LOOPS
    rsr     a2, XT_REG_LBEG
    s32i    a2, a0, 12
    rsr     a2, XT_REG_LEND
    s32i    a2, a0, 16
    /* save and disable any active zero-overhead loop while C runs */
    movi    a2, 0
    xsr     a2, XT_REG_LCOUNT
    s32i    a2, a0, 20
    #endif
    rsr     a2, EPC1
    s32i    a2, a0, 24

    /* exception mode on, window overflow off, one level above ours */
    movi    a0, PS_INTLEVEL(RMT_HI_LEVEL + 1) | PS_EXCM
    wsr     a0, XT_REG_PS
    rsync

    /* the remaining 60 physical registers, 12 per window rotation, 5 rotations */
    movi    a1, 5
    movi    a3, _rmt_hi_save_ctx + 4 * 4
1:
    s32i    a4, a3, 0
    s32i    a5, a3, 4
    s32i    a6, a3, 8
    s32i    a7, a3, 12
    s32i    a8, a3, 16
    s32i    a9, a3, 20
    s32i    a10, a3, 24
    s32i    a11, a3, 28
    s32i    a12, a3, 32
    s32i    a13, a3, 36
    s32i    a14, a3, 40
    s32i    a15, a3, 44
    addi    a13, a1, -1
    addi    a15, a3, 48
    beqz    a13, 2f
    rotw    3
    j       1b
2:
    rotw    4
    /* all registers saved: WINDOWSTART = 1 << WINDOWBASE */
    rsr     a2, XT_REG_WINDOWBASE
    movi    a3, 1
    ssl     a2
    sll     a3, a3
    wsr     a3, XT_REG_WINDOWSTART

_rmt_hi_stack_switch:
    movi    a0, 0
    movi    sp, _rmt_hi_intr_stack + RMT_HI_INTR_STACK - 16
    s32e    a0, sp, -12
    s32e    a0, sp, -16
    movi    a0, _rmt_hi_stack_switch
    /* PS for C at our own level: interrupts below us masked, EXCM clear, windowing on */
    movi    a6, PS_INTLEVEL(RMT_HI_LEVEL) | PS_UM | PS_WOE
    wsr     a6, XT_REG_PS
    rsync

    mov     a6, sp
    call4   rmtHiIsr
    l32e    sp, sp, -12

    /* back to exception mode for the restore */
    movi    a2, PS_INTLEVEL(RMT_HI_LEVEL + 1) | PS_EXCM
    wsr     a2, XT_REG_PS
    rsync

    movi    a0, _rmt_hi_save_ctx + SPECREG_OFFSET
    l32i    a2, a0, 8
    wsr     a2, XT_REG_SAR
    #if XCHAL_HAVE_LOOPS
    l32i    a2, a0, 12
    wsr     a2, XT_REG_LBEG
    l32i    a2, a0, 16
    wsr     a2, XT_REG_LEND
    l32i    a2, a0, 20
    wsr     a2, XT_REG_LCOUNT
    #endif
    l32i    a2, a0, 24
    wsr     a2, EPC1

    /* restore the 60 registers, the reverse of the save */
    rotw    -4
    movi    a15, _rmt_hi_save_ctx + 64 * 4
    movi    a13, 5
1:
    addi    a1, a13, -1
    addi    a3, a15, -48
    l32i    a4, a3, 0
    l32i    a5, a3, 4
    l32i    a6, a3, 8
    l32i    a7, a3, 12
    l32i    a8, a3, 16
    l32i    a9, a3, 20
    l32i    a10, a3, 24
    l32i    a11, a3, 28
    l32i    a12, a3, 32
    l32i    a13, a3, 36
    l32i    a14, a3, 40
    l32i    a15, a3, 44
    beqz    a1, 2f
    rotw    -3
    j       1b
2:
    movi    a0, _rmt_hi_save_ctx
    l32i    a2, a0, SPECREG_OFFSET + 4
    wsr     a2, XT_REG_WINDOWSTART
    l32i    a1, a0, 4
    l32i    a2, a0, 8
    l32i    a3, a0, 12
    rsr     a0, XT_REG_EXCSAVE_5
    rfi     RMT_HI_LEVEL

/* xt_highint5 is only a weak default elsewhere, so nothing would make the linker keep this
 * object. This symbol is referenced from C (and with -u in CMake) to force it in. */
    .global ld_include_rmt_hi_vector
ld_include_rmt_hi_vector:

#endif /* __XTENSA__ && CONFIG_IDF_TARGET_ESP32 && !CONFIG_BTDM_CTRL_HLI */
