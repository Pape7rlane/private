#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>

int main(int argc, char** argv) {
    // ptrscan <pid> <lo> <hi> <minrun>
    if (argc < 5) { printf("usage: ptrscan <pid> <lo_hex> <hi_hex> <minrun>\n"); return 1; }
    int pid = atoi(argv[1]);
    unsigned long lo = strtoul(argv[2], NULL, 16), hi = strtoul(argv[3], NULL, 16);
    int minrun = atoi(argv[4]);
    char p[64]; snprintf(p, sizeof p, "/proc/%d/maps", pid);
    FILE* f = fopen(p, "r"); if (!f) { perror("maps"); return 2; }
    snprintf(p, sizeof p, "/proc/%d/mem", pid);
    int mem = open(p, O_RDONLY); if (mem < 0) { perror("mem"); return 3; }

    const size_t CH = 4u << 20;
    uint64_t* buf = malloc(CH);
    char line[512];

    while (fgets(line, sizeof line, f)) {
        unsigned long start = 0, end = 0; char perms[8] = {0};
        if (sscanf(line, "%lx-%lx %7s", &start, &end, perms) != 3) continue;
        if (perms[0] != 'r') continue;
        // 只看私有匿名或 relro, 跳过文件映射的只读段
        if (strstr(line, " r--p ") && strstr(line, "libil2cpp.so")) continue;
        unsigned long size = end - start;
        if (size < 4096) continue;
        for (unsigned long off = 0; off < size; off += CH) {
            size_t want = size - off < CH ? size - off : CH;
            ssize_t n = pread(mem, buf, want, (off_t)(start + off));
            if (n <= 0) break;
            ssize_t cnt = n / 8;
            ssize_t run = 0;
            for (ssize_t i = 0; i < cnt; i++) {
                uint64_t v = buf[i];
                if (v >= lo && v < hi) { run++; }
                else {
                    if (run >= minrun)
                        printf("RUN len=%zd at 0x%lx  %s", run, start + off + (i - run) * 8, line);
                    run = 0;
                }
            }
            if (run >= minrun) printf("RUN len=%zd at 0x%lx (tail) %s", run, start + off + (cnt - run) * 8, line);
            if (n < (ssize_t)want) break;
        }
    }
    return 0;
}
