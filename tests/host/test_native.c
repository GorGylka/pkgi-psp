#include <assert.h>
#include <string.h>
#include <stdio.h>
#include <psphttp.h>
#include "pkgi_native_http.h"
static int fail_step, step, live, status=200, unknown, error_read, cancelled, short_write;
static int position, body=250001;
static unsigned long long declared=250001;
static size_t written;
static int call(void) { return ++step == fail_step ? -1 : 0; }
int sceUtilityLoadNetModule(int x) { (void)x; return call(); }
int sceUtilityUnloadNetModule(int x) { (void)x; return 0; }
int sceHttpInit(unsigned int x) { (void)x; return call(); }
int sceHttpEnd(void) { return 0; }
int scePowerTick(int x) { (void)x; return 0; }
int sceHttpCreateTemplate(char*a,int b,int c) { (void)a;(void)b;(void)c; if(call()<0)return -1; ++live;return 1; }
#define SETTER(name) int name(int a,unsigned int b){(void)a;(void)b;return call();}
SETTER(sceHttpSetResolveTimeOut)
SETTER(sceHttpSetConnectTimeOut)
SETTER(sceHttpSetSendTimeOut)
SETTER(sceHttpSetRecvTimeOut)
int sceHttpEnableRedirect(int x){(void)x;return call();}
int sceHttpCreateConnectionWithURL(int a,const char*b,int c){(void)a;(void)b;(void)c;if(call()<0)return -1;++live;return 2;}
int sceHttpCreateRequestWithURL(int a,PspHttpMethod b,char*c,SceULong64 d){(void)a;(void)b;(void)c;(void)d;if(call()<0)return -1;++live;return 3;}
int sceHttpSendRequest(int a,void*b,unsigned int c){(void)a;(void)b;(void)c;return call();}
int sceHttpGetStatusCode(int a,int*b){(void)a;*b=status;return call();}
int sceHttpGetContentLength(int a,SceULong64*b){(void)a;*b=declared;return unknown?-1:0;}
int sceHttpDeleteTemplate(int a){(void)a;--live;return 0;}
int sceHttpDeleteConnection(int a){(void)a;--live;return 0;}
int sceHttpDeleteRequest(int a){(void)a;--live;return 0;}
int sceHttpReadData(int a,void*b,unsigned int c){(void)a;if(error_read)return -1;int n=body-position;if(n>(int)c)n=c;memset(b,0x55,n);position+=n;return n;}
static size_t writer(void*p,size_t s,size_t n,void*u){assert(u==(void*)123);assert(((char*)p)[0]==0x55);written+=s*n;return short_write?0:s*n;}
static int progress(void*p,int64_t a,int64_t b,int64_t c,int64_t d){(void)p;(void)a;(void)b;(void)c;(void)d;return cancelled;}
static int read_case(void){pkgi_native_http h;position=0;written=0;assert(pkgi_native_http_open(&h,"http://example.test/file"));int r=pkgi_native_http_read(&h,writer,(void*)123,progress);pkgi_native_http_close(&h);assert(live==0);return r;}
int main(void){
 for(int i=1;i<=4;i++){step=0;fail_step=i;assert(!pkgi_native_http_init());}
 step=0;fail_step=0;assert(pkgi_native_http_init());
 for(int i=1;i<=10;i++){pkgi_native_http h;step=0;fail_step=i;assert(!pkgi_native_http_open(&h,"http://example.test"));assert(live==0);}
 fail_step=0;assert(read_case());assert(written==250001);
 declared++;assert(!read_case());declared-=2;assert(!read_case());declared++;
 error_read=1;assert(!read_case());error_read=0;
 cancelled=1;assert(!read_case());assert(written==0);cancelled=0;
 short_write=1;assert(!read_case());short_write=0;
 unknown=1;assert(read_case());unknown=0;
 body=0;declared=0;assert(read_case());
 status=404;pkgi_native_http h;assert(!pkgi_native_http_open(&h,"http://example.test"));assert(live==0);
 pkgi_native_http_end();puts("native HTTP: success, truncated/oversize bodies, unknown length, empty response, cancellation, read/write errors, HTTP errors and cleanup PASS");
}
