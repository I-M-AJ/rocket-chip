/*
 * Bare-Metal RV64 Verification Suite for Rocket Chip TLCustomAnomalyIP
 * Target Config: freechips.rocketchip.system.AnomalySoCConfig
 *
 * Verifies:
 *   [Test 1] TileLink MMIO 32-bit Register Write & Readback (+0x04, +0x10)
 *   [Test 2] Polling-mode 16-cycle hardware compute + W1C status clear
 *   [Test 3] End-to-end PLIC Hardware Interrupt (TLCustomAnomalyIP -> ibus -> PLIC -> MEIP)
 *   [Test 4] Hardware Anomaly Threshold Detector (+0x10 threshold, +0x14 count, bit[1] W1C)
 */

#include <stdint.h>

/* =========================================================================
 * HTIF Console & Simulation Exit (libfesvr compatible, zero UART hardware needed)
 * ========================================================================= */
volatile uint64_t tohost   __attribute__((section(".tohost"), aligned(64))) = 0;
volatile uint64_t fromhost __attribute__((section(".tohost"), aligned(64))) = 0;

static void htif_putc(char ch)
{
    while (tohost != 0) {
        if (fromhost != 0)
            fromhost = 0;
    }
    tohost = ((uint64_t)1 << 56) | ((uint64_t)1 << 48) | (uint8_t)ch;
    while (tohost != 0) {
        if (fromhost != 0)
            fromhost = 0;
    }
    if (fromhost != 0)
        fromhost = 0;
}

void htif_exit(int exit_code)
{
    while (tohost != 0) {
        if (fromhost != 0)
            fromhost = 0;
    }
    tohost = ((uint64_t)(uint32_t)exit_code << 1) | 1ULL;
    while (1) {
        asm volatile ("wfi");
    }
}

static void print_str(const char *s)
{
    while (*s) {
        htif_putc(*s++);
    }
}

static void print_hex32(uint32_t val)
{
    const char hex[] = "0123456789ABCDEF";
    print_str("0x");
    for (int i = 28; i >= 0; i -= 4) {
        htif_putc(hex[(val >> i) & 0xF]);
    }
}

static void print_dec(uint32_t val)
{
    char buf[12];
    int idx = 0;
    if (val == 0) {
        htif_putc('0');
        return;
    }
    while (val > 0) {
        buf[idx++] = (char)('0' + (val % 10));
        val /= 10;
    }
    while (idx > 0) {
        htif_putc(buf[--idx]);
    }
}

/* =========================================================================
 * Rocket Chip Memory Map: TLCustomAnomalyIP & SiFive PLIC
 * ========================================================================= */
#define CUSTOM_IP_BASE              0x10020000UL
#define CUSTOM_IP_CTRL              (*(volatile uint32_t *)(CUSTOM_IP_BASE + 0x00UL))
#define CUSTOM_IP_DATA_IN           (*(volatile uint32_t *)(CUSTOM_IP_BASE + 0x04UL))
#define CUSTOM_IP_RESULT            (*(volatile uint32_t *)(CUSTOM_IP_BASE + 0x08UL))
#define CUSTOM_IP_IRQ_STATUS        (*(volatile uint32_t *)(CUSTOM_IP_BASE + 0x0CUL))
#define CUSTOM_IP_ANOMALY_THRESHOLD (*(volatile uint32_t *)(CUSTOM_IP_BASE + 0x10UL))
#define CUSTOM_IP_ANOMALY_COUNT     (*(volatile uint32_t *)(CUSTOM_IP_BASE + 0x14UL))

#define CTRL_START                  (1U << 0)
#define CTRL_IRQ_EN                 (1U << 1)
#define CTRL_BUSY                   (1U << 2)

#define IRQ_DONE_PENDING            (1U << 0)
#define IRQ_ANOMALY_PENDING         (1U << 1)

/* Rocket Chip PLIC @ 0x0C00_0000 (Context 0 = Hart 0 M-Mode) */
#define PLIC_BASE                   0x0C000000UL
#define PLIC_PRIORITY(id)           (*(volatile uint32_t *)(PLIC_BASE + 4UL * (id)))
#define PLIC_PENDING                (*(volatile uint32_t *)(PLIC_BASE + 0x001000UL))
#define PLIC_MENABLE_0              (*(volatile uint32_t *)(PLIC_BASE + 0x002000UL))
#define PLIC_MTHRESHOLD_0           (*(volatile uint32_t *)(PLIC_BASE + 0x200000UL))
#define PLIC_MCLAIM_0               (*(volatile uint32_t *)(PLIC_BASE + 0x200004UL))

/* RISC-V M-Mode CSR bits */
#define MSTATUS_MIE                 (1UL << 3)
#define MIE_MEIE                    (1UL << 11)
#define MCAUSE_INTR_BIT             (1ULL << 63)
#define MCAUSE_M_EXT_IRQ            (MCAUSE_INTR_BIT | 11ULL)

/* =========================================================================
 * Shared Trap State
 * ========================================================================= */
static volatile uint32_t g_irq_fired       = 0;
static volatile uint32_t g_last_claim_id   = 0;
static volatile uint32_t g_last_irq_status = 0;

void m_trap_handler(uint64_t mcause, uint64_t mepc)
{
    if (mcause == MCAUSE_M_EXT_IRQ) {
        /* 1. Claim highest-priority pending interrupt from PLIC */
        uint32_t claim_id = PLIC_MCLAIM_0;
        g_last_claim_id = claim_id;

        if (claim_id != 0) {
            /* 2. Read & clear W1C interrupt pending bits inside Custom Anomaly IP */
            uint32_t status = CUSTOM_IP_IRQ_STATUS;
            g_last_irq_status = status;
            CUSTOM_IP_IRQ_STATUS = (IRQ_DONE_PENDING | IRQ_ANOMALY_PENDING);
            /* Read back to ensure TileLink PutFullData ack completes before PLIC complete */
            (void)CUSTOM_IP_IRQ_STATUS;

            /* 3. Signal completion to PLIC LevelGateway */
            PLIC_MCLAIM_0 = claim_id;
            g_irq_fired++;
        }
    } else {
        print_str("\n[TRAP ERROR] Unexpected synchronous/async trap! mcause=");
        print_hex32((uint32_t)(mcause >> 32));
        print_hex32((uint32_t)mcause);
        print_str(" mepc=");
        print_hex32((uint32_t)mepc);
        print_str("\n");
        htif_exit(99);
    }
}

static void plic_init_for_custom_ip(void)
{
    /* Enable PLIC source IDs 1..4 (ExtInterrupts [1..2] are tied 0 in TestHarness;
       TLCustomAnomalyIP is source ID 3, or ID 1 if NExtTopInterrupts == 0) */
    for (uint32_t id = 1; id <= 4; id++) {
        PLIC_PRIORITY(id) = 1;
    }
    PLIC_MENABLE_0    = (1U << 1) | (1U << 2) | (1U << 3) | (1U << 4);
    PLIC_MTHRESHOLD_0 = 0;
}

/* =========================================================================
 * Main Verification Suite
 * ========================================================================= */
int main(void)
{
    int errors = 0;

    print_str("\n============================================================\n");
    print_str("  Rocket Chip Custom Anomaly IP Verification (RV64 Bare-Metal)\n");
    print_str("  MMIO Base: 0x10020000 | PLIC Base: 0x0C000000\n");
    print_str("============================================================\n");

    /* ---------------------------------------------------------------------
     * [Test 1] TileLink 32-bit Register Write / Readback
     * --------------------------------------------------------------------- */
    print_str("\n[Test 1] TileLink MMIO Register Read/Write...\n");
    CUSTOM_IP_DATA_IN = 0x12345678U;
    uint32_t rb1 = CUSTOM_IP_DATA_IN;
    print_str("  Wrote 0x12345678 -> Read DATA_IN: ");
    print_hex32(rb1);
    if (rb1 == 0x12345678U) {
        print_str(" [PASS]\n");
    } else {
        print_str(" [FAIL]\n");
        errors++;
    }

    CUSTOM_IP_ANOMALY_THRESHOLD = 0xCAFEBABEU;
    uint32_t rb2 = CUSTOM_IP_ANOMALY_THRESHOLD;
    print_str("  Wrote 0xCAFEBABE -> Read ANOMALY_THRESHOLD: ");
    print_hex32(rb2);
    if (rb2 == 0xCAFEBABEU) {
        print_str(" [PASS]\n");
    } else {
        print_str(" [FAIL]\n");
        errors++;
    }
    /* Reset threshold to 0 (disabled) for quasiSoC-compatible Tests 2 & 3 */
    CUSTOM_IP_ANOMALY_THRESHOLD = 0U;
    CUSTOM_IP_ANOMALY_COUNT     = 0U;

    /* ---------------------------------------------------------------------
     * [Test 2] Polling-Mode 16-Cycle Compute & W1C Clear
     * --------------------------------------------------------------------- */
    print_str("\n[Test 2] Polling-Mode 16-Cycle Hardware Compute...\n");
    CUSTOM_IP_IRQ_STATUS = 0x3U;
    CUSTOM_IP_DATA_IN    = 0x12345678U;
    CUSTOM_IP_CTRL       = CTRL_START; /* irq_enable = 0, start = 1 */

    uint32_t poll_iters = 0;
    while ((CUSTOM_IP_CTRL & CTRL_BUSY) != 0U) {
        poll_iters++;
    }

    uint32_t expected2 = (0x12345678U ^ 0xA5A55A5AU) + 1U; /* 0xB7910C23 */
    uint32_t actual2   = CUSTOM_IP_RESULT;
    uint32_t status2   = CUSTOM_IP_IRQ_STATUS;

    print_str("  Poll iterations while busy: ");
    print_dec(poll_iters);
    print_str("\n  Expected RESULT: ");
    print_hex32(expected2);
    print_str(" | Actual RESULT: ");
    print_hex32(actual2);
    if (actual2 == expected2) {
        print_str(" [PASS]\n");
    } else {
        print_str(" [FAIL]\n");
        errors++;
    }

    /* Clear done_pending via W1C and verify it returns to 0 */
    CUSTOM_IP_IRQ_STATUS = IRQ_DONE_PENDING;
    uint32_t status2_after = CUSTOM_IP_IRQ_STATUS;
    print_str("  IRQ_STATUS before W1C: ");
    print_hex32(status2);
    print_str(" | after W1C: ");
    print_hex32(status2_after);
    if (((status2 & IRQ_DONE_PENDING) != 0U) && ((status2_after & IRQ_DONE_PENDING) == 0U)) {
        print_str(" [PASS]\n");
    } else {
        print_str(" [FAIL]\n");
        errors++;
    }

    /* ---------------------------------------------------------------------
     * [Test 3] End-to-End PLIC Hardware Interrupt Test
     * --------------------------------------------------------------------- */
    print_str("\n[Test 3] Hardware Interrupt via Rocket PLIC (MEIP)...\n");
    plic_init_for_custom_ip();
    g_irq_fired = 0;

    /* Enable M-mode external interrupts in CPU CSRs */
    asm volatile ("csrs mie, %0" :: "r"(MIE_MEIE));
    asm volatile ("csrs mstatus, %0" :: "r"(MSTATUS_MIE));

    CUSTOM_IP_DATA_IN = 0xDEADBEEFU;
    CUSTOM_IP_CTRL    = (CTRL_IRQ_EN | CTRL_START);

    uint32_t timeout = 100000U;
    while ((g_irq_fired == 0U) && (timeout > 0U)) {
        timeout--;
    }

    uint32_t expected3 = (0xDEADBEEFU ^ 0xA5A55A5AU) + 1U; /* 0x7B08E4B6 */
    uint32_t actual3   = CUSTOM_IP_RESULT;

    if (g_irq_fired == 1U) {
        print_str("  IRQ received! PLIC claim_id = ");
        print_dec(g_last_claim_id);
        print_str(" | captured IRQ_STATUS = ");
        print_hex32(g_last_irq_status);
        print_str(" [PASS]\n");
    } else {
        print_str("  TIMEOUT waiting for PLIC interrupt! PLIC_PENDING = ");
        print_hex32(PLIC_PENDING);
        print_str(" [FAIL]\n");
        errors++;
    }

    print_str("  Expected RESULT: ");
    print_hex32(expected3);
    print_str(" | Actual RESULT: ");
    print_hex32(actual3);
    if (actual3 == expected3) {
        print_str(" [PASS]\n");
    } else {
        print_str(" [FAIL]\n");
        errors++;
    }

    /* ---------------------------------------------------------------------
     * [Test 4] Hardware Anomaly Threshold Detection
     * --------------------------------------------------------------------- */
    print_str("\n[Test 4] Hardware Anomaly Threshold Detection...\n");
    CUSTOM_IP_ANOMALY_THRESHOLD = 0x00001000U;
    CUSTOM_IP_ANOMALY_COUNT     = 0U;

    /* 4a: Normal sample (0x0500 <= 0x1000) -> done_pending=1, anomaly_pending=0 */
    g_irq_fired = 0;
    CUSTOM_IP_DATA_IN = 0x00000500U;
    CUSTOM_IP_CTRL    = (CTRL_IRQ_EN | CTRL_START);
    timeout = 100000U;
    while ((g_irq_fired == 0U) && (timeout > 0U)) {
        timeout--;
    }
    uint32_t normal_status = g_last_irq_status;
    uint32_t cnt_after_normal = CUSTOM_IP_ANOMALY_COUNT;

    /* 4b: Anomalous sample (0x2500 > 0x1000) -> done_pending=1, anomaly_pending=1, count=1 */
    g_irq_fired = 0;
    CUSTOM_IP_DATA_IN = 0x00002500U;
    CUSTOM_IP_CTRL    = (CTRL_IRQ_EN | CTRL_START);
    timeout = 100000U;
    while ((g_irq_fired == 0U) && (timeout > 0U)) {
        timeout--;
    }
    uint32_t anom_status = g_last_irq_status;
    uint32_t cnt_after_anom = CUSTOM_IP_ANOMALY_COUNT;

    print_str("  Normal sample (0x0500 <= 0x1000): ANOMALY_COUNT = ");
    print_dec(cnt_after_normal);
    print_str(", anomaly_bit = ");
    print_dec((normal_status & IRQ_ANOMALY_PENDING) ? 1U : 0U);
    if ((cnt_after_normal == 0U) && ((normal_status & IRQ_ANOMALY_PENDING) == 0U)) {
        print_str(" [PASS]\n");
    } else {
        print_str(" [FAIL]\n");
        errors++;
    }

    print_str("  Anomaly sample (0x2500 > 0x1000): ANOMALY_COUNT = ");
    print_dec(cnt_after_anom);
    print_str(", anomaly_bit = ");
    print_dec((anom_status & IRQ_ANOMALY_PENDING) ? 1U : 0U);
    if ((cnt_after_anom == 1U) && ((anom_status & IRQ_ANOMALY_PENDING) != 0U)) {
        print_str(" [PASS]\n");
    } else {
        print_str(" [FAIL]\n");
        errors++;
    }

    /* Disable M-mode external interrupts before exit */
    asm volatile ("csrc mstatus, %0" :: "r"(MSTATUS_MIE));
    asm volatile ("csrc mie, %0" :: "r"(MIE_MEIE));

    print_str("\n============================================================\n");
    if (errors == 0) {
        print_str("  ALL 4 CUSTOM ANOMALY IP TESTS PASSED! (errors = 0)\n");
    } else {
        print_str("  CUSTOM ANOMALY IP VERIFICATION FAILED! errors = ");
        print_dec((uint32_t)errors);
        print_str("\n");
    }
    print_str("============================================================\n\n");

    return errors;
}
