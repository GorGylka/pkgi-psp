#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <unistd.h>
#include <sys/stat.h>
#include <string.h>
#include "pspkernel.h"
#include "pkgi_pair_crypto.h"
#include "pkgi.h"
#include "pkgi_download.h"
static int mode, submitted, joined, chunks, pending;
static mbedtls_aes_context me_ctx;
static uint8_t remote[PKGI_ME_BLOCK_SIZE], remote_iv[16];
static size_t remote_size;
static uint64_t remote_offset;
static const char* root;
int sceIoOpen(const char*p,int f,int m) {return open(p,f,m);}
int sceIoClose(int fd) {return close(fd);}
SceOff sceIoLseek(int fd,SceOff off,int whence) {return lseek(fd,off,whence);}
int sceIoRemove(const char*p) {return unlink(p);}
int sceIoRead(int fd,void*p,unsigned n) {
 // Partial reads are exercised on the payload requests (metadata has tiny reads).
 if(n>4096 && mode==3)n=4096;
 return read(fd,p,n);
}
int sceIoWrite(int fd,const void*p,unsigned n) {
 if (mode==5) return -1;
 if(mode==3 && n>4096)n=4096;
 return write(fd,p,n);
}
const char* pkgi_get_storage_device(void) {return root;}
int pkgi_mkdirs(const char*p) {
 char copy[1024];snprintf(copy,sizeof(copy),"%s",p);
 for(char*q=copy+1;*q;++q)if(*q=='/') {*q=0;mkdir(copy,0777);*q='/';}
 mkdir(copy,0777);return 1;
}
void update_install_progress(const char*p,int64_t n) {(void)p;(void)n;}
int convert_psp_pkg_iso(const char*p,int c) {(void)p;(void)c;return 0;}
int pkgi_me_begin(const uint8_t key[16]) {
 mbedtls_aes_init(&me_ctx);mbedtls_aes_setkey_enc(&me_ctx,key,128);return mode!=0;
}
void pkgi_me_end(void) {assert(!pending);mbedtls_aes_free(&me_ctx);}
int pkgi_me_submit(const uint8_t*p,size_t n,const uint8_t iv[16],uint64_t off) {
 assert(!pending);assert(n<=PKGI_ME_BLOCK_SIZE);submitted++;
 if(mode==6 || (mode==17 && submitted==2))return 0;
 memcpy(remote,p,n);memcpy(remote_iv,iv,16);remote_size=n;remote_offset=off;pending=1;return 1;
}
int pkgi_me_wait(uint8_t*p,size_t n) {
 if(mode==16)usleep(25000);
 assert(pending);assert(n==remote_size);pending=0;joined++;
 if(mode==4)return 0;
 assert(pkgi_ctr_xor(&me_ctx,remote_iv,remote_offset,remote,n));memcpy(p,remote,n);chunks++;return 1;
}
int main(int argc,char**argv) {
 assert(argc==4);root=argv[2];mode=atoi(argv[3]);
 int result=install_psp_pkg(argv[1]);
 printf("result=%d submitted=%d joined=%d remote_chunks=%d\n",result,submitted,joined,chunks);
 if(mode>=4) {assert(!result);return 0;}
 assert(result);if(mode)assert(submitted>0 && submitted==joined);else assert(!submitted);
 return 0;
}

int pkgi_exit_requested(void) { return mode == 7 && chunks >= 1; }
