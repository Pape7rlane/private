#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>

int main(int argc, char** argv) {
    if (argc < 3) { printf("usage: proc_scan <pid> <needle> [min_mb]\n"); return 1; }
    int pid = atoi(argv[1]);
    const char* needle = argv[2];
    size_t nl = strlen(needle);
    long minmb = (argc > 3) ? atol(argv[3]) : 1;
    char p[64];

    snprintf(p, sizeof p, "/proc/%d/maps", pid);
    FILE* f = fopen(p, "r");
    if (!f) { perror("maps"); return 2; }
    snprintf(p, sizeof p, "/proc/%d/mem", pid);
    int mem = open(p, O_RDONLY);
    if (mem < 0) { perror("mem"); fclose(f); return 3; }

    const size_t CH = 8u << 20;
    unsigned char* buf = malloc(CH);
    if (!buf) return 4;

    char line[512];
    long scanned = 0;
    int hits = 0;
    time_t t0 = time(NULL);

    while (fgets(line, sizeof line, f)) {
        unsigned long start = 0, end = 0;
        char perms[8] = {0};
        if (sscanf(line, "%lx-%lx %7s", &start, &end, perms) != 3) continue;
        if (perms[0] != 'r') continue;
        unsigned long size = end - start;
        if (size < (unsigned long)minmb * 1024ul * 1024ul) continue;
        if (time(NULL) - t0 > 300) { printf("timeout\n"); break; }

        for (unsigned long off = 0; off < size; off += CH) {
            size_t want = size - off < CH ? size - off : CH;
            ssize_t n = pread(mem, buf, want, (off_t)(start + off));
            if (n <= 0) break;
            scanned += n;
            for (ssize_t i = 0; i + (ssize_t)nl <= n; i++) {
                if (memcmp(buf + i, needle, nl) == 0) {
                    printf("HIT 0x%lx  %s", start + off + i, line);
                    hits++;
                    if (hits >= 12) goto done;
                    i += nl;
                }
            }
            if (n < (ssize_t)want) break;
        }
    }
done:
    printf("scanned=%.1fMB hits=%d secs=%ld\n", scanned / 1048576.0, hits, (long)(time(NULL) - t0));
    return 0;
}
