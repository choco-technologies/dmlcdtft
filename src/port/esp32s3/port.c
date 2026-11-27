#define DMOD_ENABLE_REGISTRATION    ON
#include "dmlcdtft_port.h"
#include "dmod.h"
#include "dmosi.h"
#include "dmdma_lease.h"
#include <errno.h>

/*
 * dmlcdtft port of the ESP32-S3: the RGB panel is driven by the LCD_CAM peripheral in the RGB mode, the frame buffer (RGB565, in the external
 * RAM) is read by a free-running circular GDMA transfer (dmdma, a lease of a channel, PERI_SEL 5), the pins go through the GPIO matrix (the
 * pin_* keys of the ini). At every vertical sync the stream is started from the first byte of the buffer afresh (see the interrupt below), so
 * a frame that ever lacked data does not shift the picture for good. Not supported: the background colour and the alpha (no layers).
 * The bandwidth: 2 bytes per pixel clock (24 MB/s at 12 MHz) have to come from the PSRAM - it has to run at 40 MHz DDR (dmqspi), at 20 MHz the
 * picture breaks up.
 *
 * Bounce buffer (bounce_lines > 0): the LCD does not read the PSRAM itself. It is fed by the circular GDMA transfer from a buffer in the
 * internal RAM, two halves of bounce_lines lines each (a "chunk"); a second GDMA channel (memory -> memory, dmdma) copies the next chunk of the
 * frame buffer from the PSRAM into the half the LCD has just finished. The LCD then never waits for the PSRAM or the cache, only the copy has
 * to be ready within the time of one half (bounce_lines lines). Every vertical sync the ring starts from the beginning of the buffer
 * (halves 0 and 1 hold the chunks 0 and 1 of the frame by then); when the copies were late or the events did not add up, the two halves are
 * loaded again first.
 */

#define SYSTEM_BASE         0x600C0000UL
#define PERIP_CLK_EN1       (SYSTEM_BASE + 0x1CUL)
#define PERIP_RST_EN1       (SYSTEM_BASE + 0x24UL)
#define LCD_CAM_BIT         (1UL << 8)
#define LCD_BASE            0x60041000UL
#define LCD_CLOCK           (LCD_BASE + 0x00UL)
#define LCD_USER            (LCD_BASE + 0x14UL)
#define LCD_MISC            (LCD_BASE + 0x18UL)
#define LCD_CTRL            (LCD_BASE + 0x1CUL)
#define LCD_CTRL1           (LCD_BASE + 0x20UL)
#define LCD_CTRL2           (LCD_BASE + 0x24UL)
#define LCD_INT_ENA         (LCD_BASE + 0x64UL)
#define LCD_INT_RAW         (LCD_BASE + 0x68UL)
#define LCD_INT_CLR         (LCD_BASE + 0x70UL)
#define LCD_INT_VSYNC       (1UL << 0)
#define LCD_MISC_AFIFO_RESET (1UL << 27)

#define GPIO_BASE           0x60004000UL
#define GPIO_ENABLE_W1TS    (GPIO_BASE + 0x24UL)
#define GPIO_ENABLE1_W1TS   (GPIO_BASE + 0x30UL)
#define GPIO_FUNC_OUT_SEL(n) (GPIO_BASE + 0x554UL + 4UL * (n))
#define IO_MUX_PIN(n)       (0x60009004UL + 4UL * (n))
#define SIG_DATA0           133U
#define SIG_DE              150U
#define SIG_HSYNC           151U
#define SIG_VSYNC           152U
#define SIG_PCLK            154U

#define INTMTX_LCD_CAM      (0x600C2000UL + 4UL * 24UL)
#define CPU_LINE            18U
#define PERI_SEL_LCD_CAM    5U
#define GDMA_CONTROLLER     0U
#define GDMA_STREAMS        5U
#define PLL_HZ              160000000UL
#define FB_ALIGN            32U
#define PSRAM_START         0x3C000000UL
#define PSRAM_END           0x3E000000UL
#define BOUNCE_JOBS         4U
#define BOUNCE_ALIGN        64U                 /* the block of the access to the external RAM (the biggest one): a chunk is a multiple of it */
#define ROM_CACHE_WRITEBACK ((uint32_t (*)(uint32_t, uint32_t))0x400016c8UL)

typedef struct
{
    uint16_t            chunk;                  /* which chunk of the frame */
    uint8_t             half;                   /* into which half of the bounce buffer */
} fill_job_t;

typedef struct
{
    bool                inited;
    bool                running;
    dmlcdtft_config_t   config;
    dmdma_lease_t       lease;
    const void         *fb;
    size_t              fb_bytes;
    uint32_t            pixel_clock;
    volatile uint32_t   frames;
    volatile uint32_t   dma_errors;
    volatile uint32_t   resyncs;                /* how many times the DMA was brought back to the start of the frame */
    volatile bool       stop;
    volatile bool       hold;                   /* the DMA is being replaced: the interrupt leaves it alone */
    dmdma_transfer_config_t transfer;       /* what the interrupt starts again at every frame */
    /* the bounce buffer (bounce == NULL: the LCD reads the frame buffer itself) */
    uint8_t            *bounce;
    uint32_t            chunk_bytes;            /* bounce_lines lines */
    uint32_t            chunks;                 /* in a frame (even) */
    dmdma_lease_t       fill_lease;             /* the copy from the external RAM */
    volatile uint32_t   consumed;               /* chunks the ring has taken since it was started at a vertical sync */
    volatile bool       out_of_step;            /* the events did not add up */
    volatile bool       fill_busy;
    volatile uint32_t   late;                   /* a half was finished while the copy of the previous one was still running */
    fill_job_t          jobs[BOUNCE_JOBS];
    volatile uint32_t   job_head;
    volatile uint32_t   job_count;
} lcd_t;


static lcd_t lcd;

static inline volatile uint32_t *reg(uintptr_t a) { return (volatile uint32_t *)a; }

static void route_out(int pad, uint32_t signal)
{
    if (pad < 0)
        return;
    if (pad == 19 || pad == 20)
        return;                                         /* the USB pads: not here */
    volatile uint32_t *mux = reg(IO_MUX_PIN((uint32_t)pad));
    *mux = (*mux & ~(7UL << 12)) | (1UL << 12) | (2UL << 10);   /* GPIO function, the drive strength 2 */
    *reg(GPIO_FUNC_OUT_SEL((uint32_t)pad)) = signal | (1UL << 10);  /* OEN_SEL: the output enable is the register's */
    if (pad < 32)
        *reg(GPIO_ENABLE_W1TS) = 1UL << pad;
    else
        *reg(GPIO_ENABLE1_W1TS) = 1UL << (pad - 32);
}

static void route_pins(const dmlcdtft_config_t *c)
{
    for (unsigned i = 0; i < 16U; i++)
        route_out(c->data_pins[i], SIG_DATA0 + i);
    route_out(c->de_pin, SIG_DE);
    route_out(c->hsync_pin, SIG_HSYNC);
    route_out(c->vsync_pin, SIG_VSYNC);
    route_out(c->pclk_pin, SIG_PCLK);
}

/* lcd_clk = PLL / (n + b/a); the pixel clock is lcd_clk (CLK_EQU_SYSCLK) */
static uint32_t clock_register(uint32_t hz, uint32_t *real)
{
    uint32_t n = (uint32_t)(PLL_HZ / hz), best_a = 1, best_b = 0;
    uint64_t best_err = ~0ULL;
    uint64_t rem = PLL_HZ - (uint64_t)n * hz;           /* the fraction rem/hz of a divider step */
    if (n < 2U)
        n = 2U;
    for (uint32_t a = 1; a <= 63U; a++)
        for (uint32_t b = 0; b < a; b++)
        {
            uint64_t freq = (uint64_t)PLL_HZ * a / ((uint64_t)n * a + b);
            uint64_t err = freq > hz ? freq - hz : hz - freq;
            if (err < best_err) { best_err = err; best_a = a; best_b = b; }
        }
    (void)rem;
    *real = (uint32_t)((uint64_t)PLL_HZ * best_a / ((uint64_t)n * best_a + best_b));
    return (3UL << 29) | ((best_a & 63UL) << 23) | ((best_b & 63UL) << 17) | ((n & 255UL) << 9) | (1UL << 6) | (1UL << 0);
}

static void configure_lcd(const dmlcdtft_config_t *c)
{
    const dmlcdtft_timing_t *t = &c->timing;
    uint32_t real = 0;
    uint32_t clk = clock_register(t->pixel_clock_hz, &real);
    lcd.pixel_clock = real;

    *reg(LCD_CLOCK) = clk | (t->pclk_inverted ? (1UL << 8) : (1UL << 7));  /* the idle edge of the clock: low; the data on the falling edge */
    uint32_t ht = t->hsync_width + t->hback_porch + c->width + t->hfront_porch;
    uint32_t vt = t->vsync_width + t->vback_porch + c->height + t->vfront_porch;
    *reg(LCD_CTRL) = (t->hback_porch + t->hsync_width - 1U) | ((c->height - 1UL) << 11) | ((vt - 1UL) << 21) | (1UL << 31);
    *reg(LCD_CTRL1) = (t->vback_porch + t->vsync_width - 1U) | ((c->width - 1UL) << 8) | ((ht - 1UL) << 20);
    *reg(LCD_CTRL2) = (t->vsync_width - 1U) | (t->vsync_polarity ? 0UL : (1UL << 7)) | (1UL << 9) |
                      ((t->hsync_width - 1UL) << 16) | (t->hsync_polarity ? 0UL : (1UL << 23)) | (1UL << 24) |
                      (t->de_polarity ? 0UL : (1UL << 8));
    *reg(LCD_MISC) = (1UL << 6) | (1UL << 12) | (1UL << 25) | (1UL << 26);  /* blank cycles 1, next frame, blank enabled */
    *reg(LCD_USER) = (1UL << 13) | (1UL << 23) | (1UL << 24);               /* always out, 2 bytes, data phase */
    *reg(LCD_USER) |= 1UL << 20;                                            /* UPDATE */
}

static void intenable(uint32_t line, bool on)
{
    uint32_t v;
    __asm__ __volatile__("rsr.intenable %0" : "=a"(v));
    v = on ? (v | (1UL << line)) : (v & ~(1UL << line));
    __asm__ __volatile__("wsr.intenable %0; rsync" ::"a"(v) : "memory");
}

static int start_dma(const void *fb)
{
    const bool bounce = lcd.bounce != NULL;
    dmdma_transfer_config_t t = {
        .direction = dmdma_direction_memory_to_peripheral, .request = PERI_SEL_LCD_CAM,
        .source_address = bounce ? (const void *)lcd.bounce : fb, .destination_address = (void *)LCD_BASE,   /* ignored by the port: the peripheral side has no address */
        .source_width = dmdma_data_width_byte, .destination_width = dmdma_data_width_byte,
        .source_increment = true, .destination_increment = false, .circular = true,
        .priority = dmdma_priority_very_high, .element_count = bounce ? 2U * lcd.chunk_bytes : lcd.fb_bytes, .timeout_ms = 0,
    };
    lcd.transfer = t;
    return dmdma_lease_start(lcd.lease, &t);
}

/* ---- the bounce buffer ---- */

/* Copies the chunk `chunk` of the frame buffer into the half `half` of the bounce buffer */
static int fill_start(uint32_t half, uint32_t chunk)
{
    dmdma_transfer_config_t t = {
        .direction = dmdma_direction_memory_to_memory, .request = 0,
        .source_address = (const uint8_t *)lcd.fb + (size_t)chunk * lcd.chunk_bytes,
        .destination_address = lcd.bounce + (size_t)half * lcd.chunk_bytes,
        .source_width = dmdma_data_width_byte, .destination_width = dmdma_data_width_byte,
        .source_increment = true, .destination_increment = true, .circular = false,
        .priority = dmdma_priority_high, .element_count = lcd.chunk_bytes, .timeout_ms = 0,
    };
    lcd.fill_busy = true;
    int ret = dmdma_lease_start(lcd.fill_lease, &t);
    if (ret != 0)
    {
        lcd.fill_busy = false;
        lcd.dma_errors++;
    }
    return ret;
}

/* The next copy waiting, when the channel is free (the interrupts; a thread only while the LCD is stopped) */
static void jobs_run(void)
{
    for (uint32_t tries = 0; tries < BOUNCE_JOBS && !lcd.fill_busy && lcd.job_count != 0U; tries++)
    {
        fill_job_t job = lcd.jobs[lcd.job_head];
        lcd.job_head = (lcd.job_head + 1U) % BOUNCE_JOBS;
        lcd.job_count--;
        (void)fill_start(job.half, job.chunk);
    }
}

static void job_push(uint32_t half, uint32_t chunk)
{
    if (lcd.job_count >= BOUNCE_JOBS)
    {
        lcd.late++;
        return;
    }
    fill_job_t *job = &lcd.jobs[(lcd.job_head + lcd.job_count) % BOUNCE_JOBS];
    job->half = (uint8_t)half;
    job->chunk = (uint16_t)chunk;
    lcd.job_count++;
    jobs_run();
}

static void fill_callback(dmdma_lease_t lease, dmdma_event_t event, void *user)
{
    if ((event & (dmdma_event_error | dmdma_event_timeout)) != 0)
        lcd.dma_errors++;
    if ((event & (dmdma_event_complete | dmdma_event_error | dmdma_event_timeout)) == 0)
        return;                                                     /* (aborted: the owner of the transfer knows) */
    lcd.fill_busy = false;
    if (!lcd.hold)
        jobs_run();
}

/* The LCD has finished the half `half`: the next chunk for it (two chunks on) is copied */
static void ring_took(uint32_t half)
{
    uint32_t taken = lcd.consumed;
    lcd.consumed = taken + 1U;
    if ((taken & 1U) != half)
        lcd.out_of_step = true;
    if (lcd.fill_busy)
        lcd.late++;
    job_push(half, (taken + 2U) % lcd.chunks);
}

static void ring_callback(dmdma_lease_t lease, dmdma_event_t event, void *user)
{
    if ((event & (dmdma_event_error | dmdma_event_timeout)) != 0)
        lcd.dma_errors++;
    if (lcd.hold)
        return;
    if ((event & dmdma_event_half_complete) != 0)
        ring_took(0U);
    if ((event & dmdma_event_complete) != 0)
        ring_took(1U);
}

/* The chunks 0 and 1 of the frame into the two halves, from a thread, while the LCD is stopped */
static int bounce_prime(void)
{
    lcd.job_count = 0;
    lcd.job_head = 0;
    for (uint32_t half = 0; half < 2U; half++)
    {
        int ret = fill_start(half, half);
        if (ret != 0)
            return ret;
        for (uint32_t waited = 0; lcd.fill_busy; waited++)
        {
            if (waited >= 100U)
            {
                dmdma_lease_abort(lcd.fill_lease);
                lcd.fill_busy = false;
                return -ETIMEDOUT;
            }
            dmosi_thread_sleep(1);
        }
    }
    lcd.consumed = lcd.chunks;                                       /* (the ring is at its beginning, as after a whole frame) */
    lcd.out_of_step = false;
    return 0;
}

/* What feeds the LCD: the bounce buffer filled and the ring started, or the free-running read of the frame buffer */
static int feed_start(const void *fb)
{
    if (lcd.bounce != NULL)
    {
        int ret = bounce_prime();
        if (ret != 0)
            return ret;
    }
    return start_dma(fb);
}

static void lcd_start(void)
{
    *reg(LCD_USER) |= 1UL << 20;
    *reg(LCD_USER) |= 1UL << 27;                                            /* START */
    lcd.running = true;
}

static void lcd_stop(void)
{
    *reg(LCD_USER) &= ~(1UL << 27);
    *reg(LCD_USER) |= 1UL << 20;
    dmdma_lease_abort(lcd.lease);
    if (lcd.fill_lease != NULL)
    {
        dmdma_lease_abort(lcd.fill_lease);
        lcd.fill_busy = false;
        lcd.job_count = 0;
    }
    lcd.running = false;
}

static void dma_callback(dmdma_lease_t lease, dmdma_event_t event, void *user)
{
    if ((event & ~(dmdma_event_complete | dmdma_event_half_complete | dmdma_event_aborted)) != 0)
        lcd.dma_errors++;      /* aborted: the restart at every vertical sync stops the channel itself */
}

/*
 * The DMA runs by itself, round and round the frame buffer, while the LCD takes the bytes it needs. If the LCD ever lacks data (the cache
 * or the flash busy) or the DMA does not begin where the frame does, the picture shifts for good. So at every vertical sync, while the LCD
 * takes no data, the stream is started from the first byte of the buffer afresh (the chain is only re-armed, nothing is allocated) and
 * the FIFO between them is emptied: every frame begins right.
 */
DMOD_IRQ_HANDLER(CPU_LINE)
{
    if ((*reg(LCD_INT_RAW) & LCD_INT_VSYNC) == 0U)
        return;
    *reg(LCD_INT_CLR) = LCD_INT_VSYNC;
    lcd.frames++;
    if (lcd.lease == NULL || lcd.hold)
        return;
    if (lcd.bounce != NULL)
    {
        /* all the chunks of the frame were taken and the ring is where it should be: it starts again from the first half (the halves
         * hold the chunks 0 and 1); if not, the two halves are loaded again first (the ring needs the first one only after the blanking) */
        bool in_step = lcd.consumed == lcd.chunks && !lcd.out_of_step;
        lcd.consumed = 0;
        lcd.out_of_step = false;
        if (!in_step)
        {
            lcd.resyncs++;
            lcd.job_count = 0;
            job_push(0U, 0U);
            job_push(1U, 1U);
        }
    }
    /* The channel is stopped (its own FIFO is emptied) BEFORE the FIFO of the LCD: a channel that still runs fills the emptied FIFO with
     * what it was sending - the end of the last frame - ahead of the new stream, and the picture is late by those pixels (the end of a line
     * shows at the beginning of the next one). */
    dmdma_lease_abort(lcd.lease);
    *reg(LCD_MISC) |= LCD_MISC_AFIFO_RESET;
    *reg(LCD_MISC) &= ~LCD_MISC_AFIFO_RESET;
    if (dmdma_lease_start(lcd.lease, &lcd.transfer) == 0)
        lcd.resyncs += lcd.bounce != NULL ? 0U : 1U;
}

static void monitor_start(void)
{
    *reg(INTMTX_LCD_CAM) = CPU_LINE;
    *reg(LCD_INT_CLR) = LCD_INT_VSYNC;
    *reg(LCD_INT_ENA) = LCD_INT_VSYNC;
    intenable(CPU_LINE, true);
}

static void monitor_stop(void)
{
    intenable(CPU_LINE, false);
    *reg(LCD_INT_ENA) = 0;
}

int dmod_init(const Dmod_Config_t *Config) { return 0; }
int dmod_deinit(void) { return 0; }

dmlcdtft_instance_t dmlcdtft_port_get_instance_count(void) { return 1; }

/* The leases and the bounce buffer (what init made, whatever of it there is) */
static void bounce_release(void)
{
    if (lcd.fill_lease != NULL)
        dmdma_lease_release(lcd.fill_lease);
    lcd.fill_lease = NULL;
    if (lcd.lease != NULL)
        dmdma_lease_release(lcd.lease);
    lcd.lease = NULL;
    if (lcd.bounce != NULL)
        Dmod_Free(lcd.bounce);
    lcd.bounce = NULL;
    lcd.fill_busy = false;
    lcd.job_count = 0;
}

int dmlcdtft_port_init(dmlcdtft_instance_t instance, const dmlcdtft_config_t *c, const void *fb)
{
    uint32_t bytes = c ? (uint32_t)c->width * c->height * 2U : 0;
    if (instance != 0 || c == NULL || fb == NULL || lcd.inited)
        return -EINVAL;
    if (c->pixel_format != DMDRVI_GFX_PIXEL_FORMAT_RGB565 || (bytes % FB_ALIGN) != 0 || ((uintptr_t)fb % FB_ALIGN) != 0)
        return -ENOTSUP;
    lcd.config = *c;
    lcd.fb = fb;
    lcd.fb_bytes = bytes;
    if (c->bounce_lines != 0U)
    {
        uint32_t chunk = (uint32_t)c->bounce_lines * c->width * 2U;
        if (c->height % c->bounce_lines != 0U || ((c->height / c->bounce_lines) & 1U) != 0U || (chunk % BOUNCE_ALIGN) != 0U)
            return -EINVAL;                         /* whole chunks in a frame, an even number of them, a multiple of the block of the PSRAM */
        lcd.bounce = Dmod_MallocTagged("main", 2U * chunk);   /* the internal RAM: the LCD reads it, not the PSRAM */
        if (lcd.bounce == NULL)
        {
            DMOD_LOG_WARN("No %u bytes of the internal RAM for the bounce buffer: the LCD reads the frame buffer itself\n", (unsigned)(2U * chunk));
        }
        else
        {
            lcd.chunk_bytes = chunk;
            lcd.chunks = c->height / c->bounce_lines;
        }
    }
    for (unsigned s = 0; s < GDMA_STREAMS && lcd.lease == NULL; s++)
        lcd.lease = dmdma_lease_acquire(GDMA_CONTROLLER, (dmdma_stream_t)s);
    for (unsigned s = 0; s < GDMA_STREAMS && lcd.bounce != NULL && lcd.fill_lease == NULL; s++)
        lcd.fill_lease = dmdma_lease_acquire(GDMA_CONTROLLER, (dmdma_stream_t)s);
    if (lcd.lease == NULL || (lcd.bounce != NULL && lcd.fill_lease == NULL))
    {
        bounce_release();
        return -EBUSY;
    }
    dmdma_lease_set_callback(lcd.lease, lcd.bounce != NULL ? ring_callback : dma_callback, NULL);
    if (lcd.fill_lease != NULL)
        dmdma_lease_set_callback(lcd.fill_lease, fill_callback, NULL);

    *reg(PERIP_CLK_EN1) |= LCD_CAM_BIT;
    *reg(PERIP_RST_EN1) |= LCD_CAM_BIT;
    *reg(PERIP_RST_EN1) &= ~LCD_CAM_BIT;
    route_pins(c);
    configure_lcd(c);
    *reg(LCD_INT_CLR) = 0xFFFFFFFFUL;
    lcd.hold = true;
    int ret = feed_start(fb);
    if (ret != 0)
    {
        bounce_release();
        return ret;
    }
    lcd_start();
    lcd.inited = true;
    lcd.hold = false;
    monitor_start();
    return 0;
}

int dmlcdtft_port_deinit(dmlcdtft_instance_t instance)
{
    if (instance != 0 || !lcd.inited)
        return -EINVAL;
    monitor_stop();
    lcd.hold = true;
    lcd_stop();
    bounce_release();
    *reg(PERIP_CLK_EN1) &= ~LCD_CAM_BIT;
    lcd.inited = false;
    return 0;
}

int dmlcdtft_port_wait_vsync(dmlcdtft_instance_t instance, uint32_t timeout_ms)
{
    if (instance != 0 || !lcd.inited)
        return -EINVAL;
    uint32_t start = lcd.frames;
    for (uint32_t waited = 0; lcd.frames == start; waited++)
    {
        if (waited >= timeout_ms)
            return -ETIMEDOUT;
        dmosi_thread_sleep(1);
    }
    return 0;
}

int dmlcdtft_port_set_framebuffer(dmlcdtft_instance_t instance, const void *fb, bool wait_vblank)
{
    if (instance != 0 || !lcd.inited || fb == NULL || ((uintptr_t)fb % FB_ALIGN) != 0)
        return -EINVAL;
    if (wait_vblank)
        (void)dmlcdtft_port_wait_vsync(instance, 100);
    lcd.hold = true;
    lcd_stop();
    lcd.fb = fb;
    int ret = feed_start(fb);
    lcd_start();
    lcd.hold = false;
    return ret;
}

int dmlcdtft_port_set_enabled(dmlcdtft_instance_t instance, bool enabled)
{
    if (instance != 0 || !lcd.inited)
        return -EINVAL;
    if (enabled && !lcd.running)
    {
        int ret = feed_start(lcd.fb);
        if (ret == 0)
            lcd_start();
        lcd.hold = false;
        return ret;
    }
    if (!enabled && lcd.running)
    {
        lcd.hold = true;
        lcd_stop();
    }
    return 0;
}

int dmlcdtft_port_set_background_color(dmlcdtft_instance_t instance, uint32_t rgb) { return -ENOTSUP; }
int dmlcdtft_port_set_alpha(dmlcdtft_instance_t instance, uint8_t alpha) { return -ENOTSUP; }

void dmlcdtft_port_sync(dmlcdtft_instance_t instance, const void *address, size_t size)
{
    uintptr_t a = (uintptr_t)address;
    if (a >= PSRAM_START && a < PSRAM_END)
        ROM_CACHE_WRITEBACK((uint32_t)a, (uint32_t)size);
}

uint32_t dmlcdtft_port_get_pixel_clock(dmlcdtft_instance_t instance) { return lcd.pixel_clock; }
/* The DMA errors, and the copies of the bounce buffer that were late (a half finished before the previous copy had) or lost */
uint32_t dmlcdtft_port_get_underrun_count(dmlcdtft_instance_t instance) { return lcd.dma_errors + lcd.late; }
