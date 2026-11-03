#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
int main(int argc,char**argv){
    if(argc<4){printf("usage: peek <pid> <addrhex> <len>\n");return 1;}
    int pid=atoi(argv[1]);
    unsigned long long addr=strtoull(argv[2],0,16);
    long len=atol(argv[3]);
    char p[64]; snprintf(p,sizeof p,"/proc/%d/mem",pid);
    int fd=open(p,O_RDONLY); if(fd<0){perror("mem");return 2;}
    unsigned char*buf=malloc(len);
    ssize_t n=pread(fd,buf,len,(off_t)addr);
    if(n<=0){printf("pread failed n=%zd\n",n);return 3;}
    for(ssize_t i=0;i<n;i+=16){
        printf("%016llx  ",addr+i);
        for(ssize_t j=0;j<16;j++){ if(i+j<n) printf("%02x ",buf[i+j]); else printf("   "); }
        printf(" |");
        for(ssize_t j=0;j<16&&i+j<n;j++){ unsigned char c=buf[i+j]; putchar(c>=32&&c<127?c:'.'); }
        printf("|\n");
    }
    return 0;
}
