#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <fcntl.h>
#include <sys/stat.h>
static unsigned long st;
static unsigned long xr(void){ st=st*6364136223846793005UL+1442695040888963407UL; return st>>11; }
static void mkname(char*b,unsigned long i,int nl){int h=snprintf(b,nl+1,"d%012lu",i);for(int k=h;k<nl;k++)b[k]='a'+((i+k)%26);b[nl]=0;}
static void dosync(const char*base){int fd=open(base,O_RDONLY|O_DIRECTORY);if(fd>=0){syncfs(fd);close(fd);}}
static double now(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return t.tv_sec+t.tv_nsec/1e9;}
int main(int argc,char**argv){
    if(argc<5){fprintf(stderr,"usage: %s BASE COUNT NAMELEN SEED [CYCLES] [SYNC_EVERY] [DELAY_MS]\n",argv[0]);return 2;}
    const char*base=argv[1];long count=atol(argv[2]);int nl=atoi(argv[3]);st=strtoul(argv[4],0,10);
    int cycles=argc>5?atoi(argv[5]):1;long se=argc>6?atol(argv[6]):0;long dly=argc>7?atol(argv[7]):0;
    char *buf=malloc(nl+1),*path=malloc(strlen(base)+nl+2);unsigned long*ord=malloc(sizeof(unsigned long)*count);
    long emap[512]; memset(emap,0,sizeof(emap)); long shown=0;
    for(int c=0;c<cycles;c++){
        for(long i=0;i<count;i++)ord[i]=i;
        for(long i=count-1;i>0;i--){unsigned long j=xr()%(i+1),t=ord[i];ord[i]=ord[j];ord[j]=t;}
        long ok=0;double t0=now();
        for(long k=0;k<count;k++){mkname(buf,ord[k],nl);sprintf(path,"%s/%s",base,buf);
            if(mkdir(path,0755)==0)ok++;else{int e=errno;if(e<512)emap[e]++;if(shown<3){printf("  MKDIR fail e=%d(%s) %s\n",e,strerror(e),buf);shown++;}}}
        printf("mkdir ok=%ld %.1fs\n",ok,now()-t0);long tot=0;for(int e=0;e<512;e++)if(emap[e]){printf("  errno %d(%s)=%ld\n",e,strerror(e),emap[e]);tot+=emap[e];}printf("  (mkdir err sum=%ld)\n",tot);
        memset(emap,0,sizeof(emap));shown=0;
        for(long i=count-1;i>0;i--){unsigned long j=xr()%(i+1),t=ord[i];ord[i]=ord[j];ord[j]=t;}
        ok=0;t0=now();
        for(long k=0;k<count;k++){mkname(buf,ord[k],nl);sprintf(path,"%s/%s",base,buf);
            if(rmdir(path)==0)ok++;else{int e=errno;if(e<512)emap[e]++;if(shown<5){printf("  RMDIR fail e=%d(%s) %s\n",e,strerror(e),buf);shown++;}}}
        printf("rmdir ok=%ld %.1fs\n",ok,now()-t0);tot=0;for(int e=0;e<512;e++)if(emap[e]){printf("  errno %d(%s)=%ld\n",e,strerror(e),emap[e]);tot+=emap[e];}printf("  (rmdir err sum=%ld)\n",tot);
    }
    dosync(base);printf("DONE\n");return 0;
}
