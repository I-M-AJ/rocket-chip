/*
 * Minimal freestanding/nolibc PID-1 Linux user-space verification program
 * for Rocket Chip RV64GC Verilator simulation.
 *
 * Built with Linux's built-in nolibc header:
 *   riscv64-unknown-linux-gnu-gcc -Os -fno-ident -fno-asynchronous-unwind-tables \
 *     -nostdlib -static -include linux/tools/include/nolibc/nolibc.h \
 *     -o rootfs/init init.c -lgcc
 */

#ifndef NOLIBC
#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mount.h>
#include <sys/utsname.h>
#include <sys/reboot.h>
#endif

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

    printf("\n[init] Phase 1 Linux boot test PASSED. Requesting SBI poweroff...\n");
    reboot(RB_POWER_OFF);
    return 0;
}
