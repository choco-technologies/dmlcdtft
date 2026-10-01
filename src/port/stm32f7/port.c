#define DMOD_ENABLE_REGISTRATION    ON
#include "dmlcdtft_port.h"
#include "dmod.h"
#include "../stm32_common/stm32_common.h"

/* ---- DMOD lifecycle ---- */

int dmod_init(const Dmod_Config_t *Config)
{
    Dmod_Printf("dmlcdtft port module initialized (stm32f7)\n");
    stm32_ltdc_common_init();
    return 0;
}

int dmod_deinit(void)
{
    Dmod_Printf("dmlcdtft port module deinitialized (stm32f7)\n");
    stm32_ltdc_common_deinit();
    return 0;
}

/* ---- Family-specific: data cache ----
 *
 * The Cortex-M7 has a data cache. When it is enabled and the framebuffer is
 * in a cacheable region, CPU writes may sit in the cache while the LTDC
 * reads stale data from memory, so the written range is cleaned by address
 * (SCB DCCMVAC, 32-byte lines). Nothing to do while the cache is off. */

#define SCB_CCR             (*(volatile uint32_t *)0xE000ED14UL)
#define SCB_CCR_DC          (1UL << 16)
#define SCB_DCCMVAC         (*(volatile uint32_t *)0xE000EF68UL)
#define DCACHE_LINE_SIZE    32U

void stm32_ltdc_family_clean_dcache(const void *address, size_t size)
{
    if ((SCB_CCR & SCB_CCR_DC) == 0U)
        return;

    uintptr_t line = (uintptr_t)address & ~(uintptr_t)(DCACHE_LINE_SIZE - 1U);
    uintptr_t end  = (uintptr_t)address + size;

    __asm volatile ("dsb" ::: "memory");
    for (; line < end; line += DCACHE_LINE_SIZE)
        SCB_DCCMVAC = (uint32_t)line;
    __asm volatile ("dsb" ::: "memory");
    __asm volatile ("isb" ::: "memory");
}

/* ---- IRQ handlers ---- */

DMOD_IRQ_HANDLER(88) { stm32_ltdc_irq_handler(); }  /* LTDC global */
DMOD_IRQ_HANDLER(89) { stm32_ltdc_irq_handler(); }  /* LTDC error */
