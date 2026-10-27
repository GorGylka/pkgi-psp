#include <errno.h>
#define main legacy_main
#include "../me/host_installer.c"
#undef main
#include "pkgi_stream.h"
int __real_stat(const char*,struct stat*);
int __wrap_stat(const char*p,struct stat*s) {
    if(!strcmp(p,"ms0:") || !strcmp(p,"ef0:")){errno=ENODEV;return -1;}
    return __real_stat(p,s);
}
int __real_mkdir(const char*,mode_t);
int __wrap_mkdir(const char*p,mode_t mode) {
    // Device root must never be treated as a regular directory.
    assert(strcmp(p,"ms0:") && strcmp(p,"ef0:"));
    return __real_mkdir(p,mode);
}
static FILE*input_file;
static uint64_t consumed,cutoff;
static unsigned writes_while_me_pending,reads_while_me_pending;
size_t __real_fwrite(const void*,size_t,size_t,FILE*);
size_t __wrap_fwrite(const void*p,size_t s,size_t n,FILE*f) {
    if(pending)++writes_while_me_pending;
    if(mode==5 && f!=stdout && f!=stderr)return 0;
    return __real_fwrite(p,s,n,f);
}
int __real_fclose(FILE*);
int __wrap_fclose(FILE*f) {
    int rc=__real_fclose(f);
    return mode==15 && f!=input_file ? -1:rc;
}
int __real_rename(const char*,const char*);
int __wrap_rename(const char*from,const char*to) {
    if(mode==10 && strstr(from,".pkgi-stream-") && !strstr(from,".old") && strstr(to,"/PSP/GAME/")){errno=EIO;return -1;}
    /* PSP rename uses the source parent even when to has another parent. */
    char actual[1024];const char *slash=strrchr(from,'/'),*base=strrchr(to,'/');
    assert(slash && base);
    snprintf(actual,sizeof(actual),"%.*s/%s",(int)(slash-from),from,base+1);
    return __real_rename(from,actual);
}
static int stream_read(void*ctx,void*buf,unsigned size) {
    (void)ctx;
    if(pending)++reads_while_me_pending;
    if(mode==8 && consumed>=cutoff)return 0;
    if(mode==3 && size>37)size=37;
    if(mode==8 && size>cutoff-consumed)size=cutoff-consumed;
    int r=fread(buf,1,size,input_file);consumed+=r;return r;
}
static int stream_finish(void*ctx){(void)ctx;return mode!=9 && fgetc(input_file)==EOF;}
static int stream_cancel(void*ctx){(void)ctx;return mode==7 && consumed>600000;}
#ifndef PKGI_PIPE_TEST
int main(int argc,char**argv) {
    assert(argc==5);root=argv[2];mode=atoi(argv[3]);int expected=atoi(argv[4]);
    input_file=fopen(argv[1],"rb");assert(input_file);fseek(input_file,0,SEEK_END);long length=ftell(input_file);rewind(input_file);cutoff=length/2;
    pkgi_stream_source s={.read=stream_read,.finish=stream_finish,.cancelled=stream_cancel,.length=length,.root=root};
    int ok=pkgi_stream_unpack(&s);fclose(input_file);
    printf("mode=%d result=%d me=%d/%d error=%s\n",mode,ok,submitted,joined,s.error);
    assert(ok==expected);assert(!pending);
    if(ok) {
        assert(s.payload_written>0);
        if(mode==0) {assert(s.cpu_bytes==s.payload_written);assert(!s.me_bytes && !s.me_jobs);}
        else {
            assert(!s.cpu_bytes && s.me_bytes==s.payload_written);
            assert(s.me_jobs==(unsigned)submitted && submitted==joined);
            assert(writes_while_me_pending && reads_while_me_pending);
        }
    }
    return 0;
}

#endif
