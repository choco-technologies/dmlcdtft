#ifndef DMLCDTFT_STM32_COMMON_H
#define DMLCDTFT_STM32_COMMON_H

#include <stdint.h>
#include <stddef.h>

/* ======================================================================
 *   Common STM32F4/F7 LTDC + RCC (PLLSAI) register definitions
 *
 *   The LCD-TFT controller is the same IP block on every STM32F4 (429/439/
 *   469/479) and STM32F7 (746/756/767/769) part that has one: same base
 *   address, register layout, IRQ numbers (88 global, 89 error), and the
 *   same clock path - the pixel clock is PLLSAI's R output divided by
 *   PLLSAIDIVR, with RCC_PLLSAICFGR at 0x88 and RCC_DCKCFGR(1) at 0x8C on
 *   both families (RM0090 / RM0385). The only difference that matters to
 *   this driver is that STM32F7's Cortex-M7 has a data cache, handled by
 *   stm32_ltdc_family_clean_dcache() in each family's port.c.
 * ====================================================================== */

/* ---- LTDC ---- */

typedef struct
{
    volatile uint32_t RESERVED0[2];
    volatile uint32_t SSCR;         /* 0x08 Synchronization size */
    volatile uint32_t BPCR;         /* 0x0C Back porch */
    volatile uint32_t AWCR;         /* 0x10 Active width */
    volatile uint32_t TWCR;         /* 0x14 Total width */
    volatile uint32_t GCR;          /* 0x18 Global control */
    volatile uint32_t RESERVED1[2];
    volatile uint32_t SRCR;         /* 0x24 Shadow reload */
    volatile uint32_t RESERVED2;
    volatile uint32_t BCCR;         /* 0x2C Background color */
    volatile uint32_t RESERVED3;
    volatile uint32_t IER;          /* 0x34 Interrupt enable */
    volatile uint32_t ISR;          /* 0x38 Interrupt status */
    volatile uint32_t ICR;          /* 0x3C Interrupt clear */
    volatile uint32_t LIPCR;        /* 0x40 Line interrupt position */
    volatile uint32_t CPSR;         /* 0x44 Current position */
    volatile uint32_t CDSR;         /* 0x48 Current display status */
} stm32_ltdc_t;

typedef struct
{
    volatile uint32_t CR;           /* +0x00 Control */
    volatile uint32_t WHPCR;        /* +0x04 Window horizontal position */
    volatile uint32_t WVPCR;        /* +0x08 Window vertical position */
    volatile uint32_t CKCR;         /* +0x0C Color keying */
    volatile uint32_t PFCR;         /* +0x10 Pixel format */
    volatile uint32_t CACR;         /* +0x14 Constant alpha */
    volatile uint32_t DCCR;         /* +0x18 Default color */
    volatile uint32_t BFCR;         /* +0x1C Blending factors */
    volatile uint32_t RESERVED0[2];
    volatile uint32_t CFBAR;        /* +0x28 Frame buffer address */
    volatile uint32_t CFBLR;        /* +0x2C Frame buffer line length */
    volatile uint32_t CFBLNR;       /* +0x30 Frame buffer line number */
    volatile uint32_t RESERVED1[3];
    volatile uint32_t CLUTWR;       /* +0x40 CLUT write */
} stm32_ltdc_layer_t;

#define STM32_LTDC_BASE             0x40016800UL
#define STM32_LTDC                  ((volatile stm32_ltdc_t *)STM32_LTDC_BASE)
#define STM32_LTDC_LAYER1           ((volatile stm32_ltdc_layer_t *)(STM32_LTDC_BASE + 0x84UL))

#define STM32_LTDC_IRQN             88U
#define STM32_LTDC_ER_IRQN          89U

#define STM32_LTDC_GCR_LTDCEN       (1U << 0)
#define STM32_LTDC_GCR_PCPOL        (1U << 28)
#define STM32_LTDC_GCR_DEPOL        (1U << 29)
#define STM32_LTDC_GCR_VSPOL        (1U << 30)
#define STM32_LTDC_GCR_HSPOL        (1U << 31)

#define STM32_LTDC_SRCR_IMR         (1U << 0)   /* Reload shadow registers now */
#define STM32_LTDC_SRCR_VBR         (1U << 1)   /* Reload at the next vertical blanking */

/* IER, ISR and ICR share the same bit positions */
#define STM32_LTDC_IT_LINE          (1U << 0)
#define STM32_LTDC_IT_FIFO_UNDERRUN (1U << 1)
#define STM32_LTDC_IT_TRANSFER_ERR  (1U << 2)
#define STM32_LTDC_IT_RELOAD        (1U << 3)
#define STM32_LTDC_IT_ALL           0xFU

#define STM32_LTDC_LxCR_LEN         (1U << 0)

/* Blending: pixel alpha x constant alpha for the layer, 1 - that for what
 * is below it - the reset value, which makes CACR the layer's opacity. */
#define STM32_LTDC_BFCR_PAXCA       ((6U << 8) | 7U)

/* Line length/number field limits */
#define STM32_LTDC_CFBLL_MAX        0x1FFFU
#define STM32_LTDC_CFBLNBR_MAX      0x7FFU

/* ---- RCC (subset, same layout on STM32F4/F7) ---- */

typedef struct
{
    volatile uint32_t CR;           /* 0x00 */
    volatile uint32_t PLLCFGR;      /* 0x04 */
    volatile uint32_t CFGR;         /* 0x08 */
    volatile uint32_t CIR;          /* 0x0C */
    volatile uint32_t AHB1RSTR;     /* 0x10 */
    volatile uint32_t AHB2RSTR;     /* 0x14 */
    volatile uint32_t AHB3RSTR;     /* 0x18 */
    volatile uint32_t RESERVED0;
    volatile uint32_t APB1RSTR;     /* 0x20 */
    volatile uint32_t APB2RSTR;     /* 0x24 */
    volatile uint32_t RESERVED1[2];
    volatile uint32_t AHB1ENR;      /* 0x30 */
    volatile uint32_t AHB2ENR;      /* 0x34 */
    volatile uint32_t AHB3ENR;      /* 0x38 */
    volatile uint32_t RESERVED2;
    volatile uint32_t APB1ENR;      /* 0x40 */
    volatile uint32_t APB2ENR;      /* 0x44 */
    volatile uint32_t RESERVED3[14];
    volatile uint32_t SSCGR;        /* 0x80 */
    volatile uint32_t PLLI2SCFGR;   /* 0x84 */
    volatile uint32_t PLLSAICFGR;   /* 0x88 */
    volatile uint32_t DCKCFGR;      /* 0x8C (DCKCFGR1 on STM32F7) */
} stm32_rcc_t;

_Static_assert(offsetof(stm32_ltdc_t, CDSR) == 0x48, "LTDC register layout");
_Static_assert(offsetof(stm32_ltdc_layer_t, CFBAR) == 0x28, "LTDC layer register layout");
_Static_assert(offsetof(stm32_ltdc_layer_t, CLUTWR) == 0x40, "LTDC layer register layout");
_Static_assert(offsetof(stm32_rcc_t, APB2ENR) == 0x44, "RCC register layout");
_Static_assert(offsetof(stm32_rcc_t, PLLSAICFGR) == 0x88, "RCC register layout");
_Static_assert(offsetof(stm32_rcc_t, DCKCFGR) == 0x8C, "RCC register layout");

#define STM32_RCC_BASE              0x40023800UL
#define STM32_RCC                   ((volatile stm32_rcc_t *)STM32_RCC_BASE)

#define STM32_RCC_CR_PLLSAION       (1U << 28)
#define STM32_RCC_CR_PLLSAIRDY      (1U << 29)

#define STM32_RCC_PLLCFGR_PLLM_Msk  0x3FU
#define STM32_RCC_PLLCFGR_PLLN_Pos  6U
#define STM32_RCC_PLLCFGR_PLLN_Msk  (0x1FFU << STM32_RCC_PLLCFGR_PLLN_Pos)
#define STM32_RCC_PLLCFGR_PLLP_Pos  16U
#define STM32_RCC_PLLCFGR_PLLP_Msk  (0x3U << STM32_RCC_PLLCFGR_PLLP_Pos)
#define STM32_RCC_PLLCFGR_PLLSRC    (1U << 22)

#define STM32_RCC_CFGR_SWS_Pos      2U
#define STM32_RCC_CFGR_SWS_Msk      (0x3U << STM32_RCC_CFGR_SWS_Pos)
#define STM32_RCC_CFGR_SWS_PLL      2U

/* Only N and R are touched - P (F7 CK48) and Q (SAI) are left as they are */
#define STM32_RCC_PLLSAICFGR_N_Pos  6U
#define STM32_RCC_PLLSAICFGR_N_Msk  (0x1FFU << STM32_RCC_PLLSAICFGR_N_Pos)
#define STM32_RCC_PLLSAICFGR_R_Pos  28U
#define STM32_RCC_PLLSAICFGR_R_Msk  (0x7U << STM32_RCC_PLLSAICFGR_R_Pos)

#define STM32_RCC_DCKCFGR_DIVR_Pos  16U
#define STM32_RCC_DCKCFGR_DIVR_Msk  (0x3U << STM32_RCC_DCKCFGR_DIVR_Pos)

#define STM32_RCC_APB2_LTDC         (1U << 26)

#define STM32_HSI_VALUE             16000000U

/* PLLSAI limits (identical in RM0090 and RM0385) */
#define STM32_PLLSAI_N_MIN          50U
#define STM32_PLLSAI_N_MAX          432U
#define STM32_PLLSAI_R_MIN          2U
#define STM32_PLLSAI_R_MAX          7U
#define STM32_PLLSAI_VCO_MIN        100000000U
#define STM32_PLLSAI_VCO_MAX        432000000U

/* ---- Interface between stm32_common.c and each family's port.c ---- */

/**
 * @brief Make CPU writes to a memory range visible to the LTDC.
 *
 * The one place the two families differ: STM32F7 cleans its data cache by
 * address (when the cache is enabled), STM32F4 has no data cache and does
 * nothing. Implemented in src/port/<family>/port.c.
 */
void stm32_ltdc_family_clean_dcache(const void *address, size_t size);

/**
 * @brief Reset the shared driver state - called from each family's
 *        dmod_init()/dmod_deinit().
 */
void stm32_ltdc_common_init(void);
void stm32_ltdc_common_deinit(void);

/**
 * @brief Shared ISR body for both LTDC interrupt lines (global and error),
 *        called from each family's DMOD_IRQ_HANDLER wrappers.
 */
void stm32_ltdc_irq_handler(void);

#endif // DMLCDTFT_STM32_COMMON_H
