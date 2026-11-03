#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>

// scan readable regions of /proc/<pid>/mem for a 4-byte magic (hex), print region info
int main(int argc, char** argv) {
    if (argc < 3) { printf("usage: mdscan <pid> <hexmagic> [dumpdir]\n"); return 1; }
    int pid = atoi(argv[1]);
    unsigned int magic = (unsigned int)strtoul(argv[2], 0, 16);
    const char* dumpdir = (argc > 3) ? argv[3] : NULL;
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
    long scanned = 0; int hits = 0; time_t t0 = time(NULL);

    while (fgets(line, sizeof line, f)) {
        unsigned long start = 0, end = 0; char perms[8] = {0};
        if (sscanf(line, "%lx-%lx %7s", &start, &end, perms) != 3) continue;
        if (perms[0] != 'r') continue;
        unsigned long size = end - start;
        if (size < 65536) continue;
        if (time(NULL) - t0 > 600) { printf("timeout\n"); break; }
        for (unsigned long off = 0; off < size; off += CH) {
            size_t want = size - off < CH ? size - off : CH;
            ssize_t n = pread(mem, buf, want, (off_t)(start + off));
            if (n <= 0) break;
            scanned += n;
            for (ssize_t i = 0; i + 4 <= n; i += 4) {
                unsigned int v; memcpy(&v, buf + i, 4);
                if (v == magic) {
                    printf("HIT 0x%lx  %s", start + off + i, line);
                    if (dumpdir) {
                        char dp[256]; snprintf(dp, sizeof dp, "%s/meta_0x%lx.bin", dumpdir, start + off + i);
                        int df = open(dp, O_WRONLY | O_CREAT | O_TRUNC, 0644);
                        if (df >= 0) {
                            // dump up to 256MB from hit
                            unsigned long left = 256ul << 20;
                            unsigned long pos = start + off + i;
                            while (left > 0) {
                                size_t w2 = left < CH ? left : CH;
                                ssize_t r2 = pread(mem, buf, w2, (off_t)pos);
                                if (r2 <= 0) break;
                                if (write(df, buf, r2) != r2) break;
                                pos += r2; left -= r2;
                            }
                            close(df);
                            printf("   dumped -> %s\n", dp);
                        }
                    }
                    hits++;
                    if (hits >= 8) goto done;
                }
            }
            if (n < (ssize_t)want) break;
        }
    }
done:
    printf("scanned=%.1fMB hits=%d secs=%ld\n", scanned / 1048576.0, hits, (long)(time(NULL) - t0));
    return 0;
}
