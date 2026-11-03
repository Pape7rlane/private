#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>

static void dump(int pid, unsigned long addr, long len) {
    char p[64]; snprintf(p, sizeof p, "/proc/%d/mem", pid);
    int mem = open(p, O_RDONLY);
    if (mem < 0) { perror("mem"); return; }
    unsigned char* buf = malloc(len);
    ssize_t n = pread(mem, buf, len, (off_t)addr);
    if (n <= 0) { perror("pread"); return; }
    for (ssize_t i = 0; i < n; i += 16) {
        printf("%016lx ", addr + i);
        for (ssize_t k = 0; k < 16; k++) printf(i + k < n ? "%02x " : "   ", buf[i + k]);
        printf("\n");
    }
}

int main(int argc, char** argv) {
    if (argc >= 5 && !strcmp(argv[1], "-d")) { dump(atoi(argv[2]), strtoul(argv[3], NULL, 16), strtol(argv[4], NULL, 0)); return 0; }
    if (argc >= 4 && !strcmp(argv[1], "-m")) {
        char p[64]; snprintf(p, sizeof p, "/proc/%d/maps", atoi(argv[2]));
        FILE* f = fopen(p, "r"); if (!f) { perror("maps"); return 2; }
        char line[512];
        while (fgets(line, sizeof line, f)) if (strstr(line, argv[3])) fputs(line, stdout);
        return 0;
    }
    printf("usage: memdump -d <pid> <hexaddr> <len> | -m <pid> <substr>\n");
    return 1;
}
