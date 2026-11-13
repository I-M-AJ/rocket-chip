/*
 * Minimal freestanding/nolibc PID-1 Linux user-space verification program
 * for Rocket Chip RV64GC Verilator simulation (Phase 1 + Phase 3 UIO).
 *
 * Built with Linux's built-in nolibc header:
 *   riscv64-unknown-linux-gnu-gcc -Os -fno-ident -fno-asynchronous-unwind-tables \
 *     -nostdlib -static -include linux/tools/include/nolibc/nolibc.h \
 *     -o rootfs/init init.c -lgcc
 */

#ifndef NOLIBC
#include <stdio.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mount.h>
#include <sys/mman.h>
#include <sys/reboot.h>
#endif

#define REG_CTRL              0x00
#define REG_DATA_IN           0x04
#define REG_RESULT            0x08
#define REG_IRQ_STATUS        0x0C
#define REG_ANOMALY_THRESHOLD 0x10
#define REG_ANOMALY_COUNT     0x14

#define CTRL_START            (1U << 0)
#define CTRL_IRQ_EN           (1U << 1)
#define CTRL_BUSY             (1U << 2)

#define IRQ_DONE_PENDING      (1U << 0)
#define IRQ_ANOMALY_PENDING   (1U << 1)

static inline uint32_t mmio_read32(volatile uint8_t *base, uint32_t offset)
{
    return *(volatile uint32_t *)(base + offset);
}

static inline void mmio_write32(volatile uint8_t *base, uint32_t offset, uint32_t val)
{
    *(volatile uint32_t *)(base + offset) = val;
}

static void dump_file(const char *path, int max_bytes)
{
    char buf[512];
    int fd = open(path, O_RDONLY, 0);
    if (fd < 0) {
        printf("[init] could not open %s\n", path);
        return;
    }
    int total = 0;
    while (total < max_bytes) {
        ssize_t n = read(fd, buf, sizeof(buf) - 1);
        if (n <= 0)
            break;
        buf[n] = '\0';
        fputs(buf, stdout);
        total += (int)n;
    }
    close(fd);
}

static int run_uio_anomaly_tests(void)
{
    int uio_fd = open("/dev/uio0", O_RDWR, 0);
    if (uio_fd < 0) {
        printf("[UIO] /dev/uio0 not found (expected on Spike; required on AnomalySoCConfig)\n");
        return 0;
    }

    volatile uint8_t *base = (volatile uint8_t *)mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, uio_fd, 0);
    if (base == MAP_FAILED) {
        printf("[UIO] ERROR: mmap(/dev/uio0) failed!\n");
        close(uio_fd);
        return 1;
    }

    int errors = 0;
    printf("\n============================================================\n");
    printf("  Phase 3: Linux User-Space /dev/uio0 Anomaly IP Tests\n");
    printf("============================================================\n");

    /* [Test 1] MMIO Register Write/Readback */
    mmio_write32(base, REG_DATA_IN, 0x12345678U);
    uint32_t rb = mmio_read32(base, REG_DATA_IN);
    printf("[UIO Test 1] DATA_IN R/W: 0x%x %s\n", rb, (rb == 0x12345678U) ? "[PASS]" : "[FAIL]");
    if (rb != 0x12345678U) errors++;

    mmio_write32(base, REG_ANOMALY_THRESHOLD, 0U);
    mmio_write32(base, REG_ANOMALY_COUNT, 0U);

    /* [Test 2] Polling-Mode 16-Cycle Compute */
    mmio_write32(base, REG_IRQ_STATUS, 0x3U);
    mmio_write32(base, REG_DATA_IN, 0x12345678U);
    mmio_write32(base, REG_CTRL, CTRL_START);
    while ((mmio_read32(base, REG_CTRL) & CTRL_BUSY) != 0U) {}
    uint32_t res2 = mmio_read32(base, REG_RESULT);
    uint32_t exp2 = (0x12345678U ^ 0xA5A55A5AU) + 1U;
    printf("[UIO Test 2] Polling RESULT: 0x%x (expected 0x%x) %s\n", res2, exp2, (res2 == exp2) ? "[PASS]" : "[FAIL]");
    if (res2 != exp2) errors++;
    mmio_write32(base, REG_IRQ_STATUS, IRQ_DONE_PENDING);

    /* [Test 3] Kernel PLIC -> /dev/uio0 Blocking Interrupt */
    uint32_t irq_en = 1U;
    uint32_t irq_cnt = 0U;
    if (write(uio_fd, &irq_en, sizeof(irq_en)) != sizeof(irq_en)) {
        printf("[UIO Test 3] ERROR: write(uio_fd, enable=1) failed\n");
        errors++;
    } else {
        mmio_write32(base, REG_DATA_IN, 0xDEADBEEFU);
        mmio_write32(base, REG_CTRL, CTRL_IRQ_EN | CTRL_START);
        ssize_t n = read(uio_fd, &irq_cnt, sizeof(irq_cnt));
        uint32_t st3 = mmio_read32(base, REG_IRQ_STATUS);
        mmio_write32(base, REG_IRQ_STATUS, IRQ_DONE_PENDING | IRQ_ANOMALY_PENDING);
        uint32_t res3 = mmio_read32(base, REG_RESULT);
        uint32_t exp3 = (0xDEADBEEFU ^ 0xA5A55A5AU) + 1U;
        printf("[UIO Test 3] IRQ read=%d cnt=%u status=0x%x RESULT=0x%x %s\n",
               (int)n, irq_cnt, st3, res3,
               (n == 4 && res3 == exp3) ? "[PASS]" : "[FAIL]");
        if (n != 4 || res3 != exp3) errors++;
    }

    /* [Test 4] Hardware Anomaly Threshold Detection */
    mmio_write32(base, REG_ANOMALY_THRESHOLD, 0x00001000U);
    mmio_write32(base, REG_ANOMALY_COUNT, 0U);

    write(uio_fd, &irq_en, sizeof(irq_en));
    mmio_write32(base, REG_DATA_IN, 0x00000500U);
    mmio_write32(base, REG_CTRL, CTRL_IRQ_EN | CTRL_START);
    read(uio_fd, &irq_cnt, sizeof(irq_cnt));
    mmio_write32(base, REG_IRQ_STATUS, IRQ_DONE_PENDING | IRQ_ANOMALY_PENDING);
    uint32_t cnt_normal = mmio_read32(base, REG_ANOMALY_COUNT);

    write(uio_fd, &irq_en, sizeof(irq_en));
    mmio_write32(base, REG_DATA_IN, 0x00002500U);
    mmio_write32(base, REG_CTRL, CTRL_IRQ_EN | CTRL_START);
    read(uio_fd, &irq_cnt, sizeof(irq_cnt));
    uint32_t st_anom = mmio_read32(base, REG_IRQ_STATUS);
    mmio_write32(base, REG_IRQ_STATUS, IRQ_DONE_PENDING | IRQ_ANOMALY_PENDING);
    uint32_t cnt_anom = mmio_read32(base, REG_ANOMALY_COUNT);

    int t4_ok = (cnt_normal == 0U) && (cnt_anom == 1U) && ((st_anom & IRQ_ANOMALY_PENDING) != 0U);
    printf("[UIO Test 4] Anomaly normal_cnt=%u anom_cnt=%u anom_bit=%u %s\n",
           cnt_normal, cnt_anom, (st_anom & IRQ_ANOMALY_PENDING) ? 1U : 0U,
           t4_ok ? "[PASS]" : "[FAIL]");
    if (!t4_ok) errors++;

    munmap((void *)base, 4096);
    close(uio_fd);
    printf("[UIO] Summary: %s (errors = %d)\n",
           (errors == 0) ? "ALL 4 LINUX UIO ANOMALY IP TESTS PASSED!" : "FAILED", errors);
    return errors;
}

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    printf("\n============================================================\n");
    printf("  HELLO FROM LINUX USER-SPACE ON ROCKET CHIP (VERILATOR)!   \n");
    printf("============================================================\n");
    printf("[init] Running as PID=%d in U-mode (Sv39 MMU active)\n", getpid());

    mkdir("/proc", 0755);
    if (mount("proc", "/proc", "proc", 0, 0) == 0) {
        printf("\n--- /proc/version ---\n");
        dump_file("/proc/version", 256);
        printf("\n--- /proc/cpuinfo ---\n");
        dump_file("/proc/cpuinfo", 1024);
        printf("--- /proc/uptime ---\n");
        dump_file("/proc/uptime", 128);
    }

    mkdir("/dev", 0755);
    mount("devtmpfs", "/dev", "devtmpfs", 0, 0);

    run_uio_anomaly_tests();

    printf("\n[init] Requesting SBI poweroff...\n");
    reboot(RB_POWER_OFF);
    return 0;
}
