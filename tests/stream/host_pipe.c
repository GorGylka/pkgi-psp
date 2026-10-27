#define PKGI_PIPE_TEST
#include "host_stream.c"
#include <pthread.h>
#include <semaphore.h>
#include <time.h>
#include <stdatomic.h>
static sem_t sems[4];static int nsems;
static pthread_t th;static int (*entry)(SceSize,void*);static void*argcopy;static unsigned argsize;
static atomic_ullong sent;
static atomic_int transport_done;
static const char* pkgpath;
int sceKernelCreateSema(const char*n,int a,int v,int max,void*x){(void)n;(void)a;(void)max;(void)x;int id=++nsems;assert(id<4);assert(!sem_init(&sems[id],0,v));return id;}
int sceKernelDeleteSema(int id){assert(!sem_destroy(&sems[id]));return 0;}
int sceKernelWaitSema(int id,int n,SceUInt*timeout){assert(n==1);struct timespec t;clock_gettime(CLOCK_REALTIME,&t);t.tv_nsec+=(long)*timeout*1000;t.tv_sec+=t.tv_nsec/1000000000;t.tv_nsec%=1000000000;return sem_timedwait(&sems[id],&t);}
int sceKernelPollSema(int id,int n){assert(n==1);return sem_trywait(&sems[id]);}
int sceKernelSignalSema(int id,int n){assert(n==1);return sem_post(&sems[id]);}
int sceKernelDelayThread(unsigned us){usleep(us);return 0;}
int sceKernelCreateThread(const char*n,int(*fn)(SceSize,void*),int p,int stack,int a,void*x){(void)n;(void)p;(void)stack;(void)a;(void)x;entry=fn;return 8;}
static void*trampoline(void*x){(void)x;entry(argsize,argcopy);return NULL;}
int sceKernelStartThread(int id,unsigned size,void*arg){(void)id;argcopy=malloc(size);memcpy(argcopy,arg,size);argsize=size;return pthread_create(&th,NULL,trampoline,NULL);}
int sceKernelWaitThreadEnd(int id,SceUInt*t){(void)id;(void)t;assert(!pthread_join(th,NULL));free(argcopy);return 0;}
int sceKernelDeleteThread(int id){(void)id;return 0;}
uint32_t pkgi_time_msec(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return t.tv_sec*1000+t.tv_nsec/1000000;}
void pkgi_dialog_start_progress(const char*a,const char*b,float c){(void)a;(void)b;(void)c;}
void pkgi_dialog_allow_close(int x){(void)x;}
void pkgi_dialog_update_progress(const char*a,const char*b,const char*c,float d){(void)a;(void)b;(void)c;(void)d;}
void progress_screen_refresh(void){}
int pkgi_dialog_is_cancelled(void){return mode==7 && atomic_load(&sent)>600000;}
void pkgi_dialog_error(const char*s){printf("UI error: %s\n",s);}
int pkgi_check_free_space(uint64_t n){(void)n;return 1;}
pkgi_http*pkgi_http_get(const char*u,const char*c,uint64_t off){(void)u;(void)c;assert(!off);return (pkgi_http*)1;}
int pkgi_http_response_length(pkgi_http*h,int64_t*len){(void)h;struct stat st;assert(!stat(pkgpath,&st));*len=st.st_size;return 1;}
void pkgi_http_close(pkgi_http*h){(void)h;assert(atomic_load(&transport_done));}
int pkgi_http_read(pkgi_http*h,void*w,void*p){
 (void)h;size_t(*writefn)(void*,size_t,size_t,void*)=w;int(*progressfn)(void*,int64_t,int64_t,int64_t,int64_t)=p;
 FILE*f=fopen(pkgpath,"rb");assert(f);uint8_t*b=malloc(700001);int ok=1;unsigned i=0;
 const unsigned chunksizes[]={1,13,102401,700001,524288,31};
 while(1){
  if(progressfn(NULL,0,0,0,0)){ok=0;break;}
  unsigned size=chunksizes[i++%6];size_t got=fread(b,1,size,f);if(!got)break;
  if(mode==8 && atomic_load(&sent)>600000){ok=0;break;}
  if(mode==3)usleep(4000);
  if(writefn(b,1,got,NULL)!=got){ok=0;break;}
  atomic_fetch_add(&sent,got);
 }
 fclose(f);free(b);atomic_store(&transport_done,1);return mode==9?0:ok;
}
int main(int argc,char**argv){
 assert(argc==5);pkgpath=argv[1];root=argv[2];mode=atoi(argv[3]);int expected=atoi(argv[4]);
 DbItem item={.name="test",.url="http://test/pkg",.content="TEST00001"};
 int ok=pkgi_stream_download(&item);assert(ok==expected);assert(!pending);assert(atomic_load(&transport_done));
 printf("PIPE PASS mode=%d result=%d sent=%llu\n",mode,ok,(unsigned long long)atomic_load(&sent));return 0;
}
