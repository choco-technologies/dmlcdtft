#define DMOD_ENABLE_REGISTRATION    ON
#include "dmlcdtft_port.h"
#include "dmod.h"
#include "../stm32_common/stm32_common.h"

/* ---- DMOD lifecycle ---- */

int dmod_init(const Dmod_Config_t *Config)
{
    Dmod_Printf("dmlcdtft port module initialized (stm32f4)\n");
    stm32_ltdc_common_init();
    return 0;
}

int dmod_deinit(void)
{
    Dmod_Printf("dmlcdtft port module deinitialized (stm32f4)\n");
    stm32_ltdc_common_deinit();
    return 0;
}

/* ---- Family-specific: data cache ----
 *
 * The Cortex-M4 has no data cache - every CPU write already reaches memory,
 * so there is nothing to clean before the LTDC reads it. Only the F4 parts
 * with an LTDC (429/439/469/479) can use this port at all. */

void stm32_ltdc_family_clean_dcache(const void *address, size_t size)
{
    (void)address;
    (void)size;
}

/* ---- IRQ handlers ---- */

DMOD_IRQ_HANDLER(88) { stm32_ltdc_irq_handler(); }  /* LTDC global */
DMOD_IRQ_HANDLER(89) { stm32_ltdc_irq_handler(); }  /* LTDC error */
