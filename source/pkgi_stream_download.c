#include "pkgi_stream.h"
#include "pkgi.h"
#include "pkgi_dialog.h"
#include "pkgi_download.h"
#include <pspkernel.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdatomic.h>

#define SLOT_SIZE (512u*1024u)
#define SLOTS 16u
/* Single producer/consumer; semaphores transfer ownership of each slot. */
typedef struct {
    pkgi_http* http;
    uint8_t* data;
    unsigned sizes[SLOTS],write_slot,fill,read_slot,read_pos,held;
    SceUID free_slots,ready,thread;
    _Atomic int abort,done,transport_ok;
    uint64_t consumed,total,received;
    uint32_t start,last_ui,http_elapsed_ms;
    const char* name;
} Pipe;
static int ui_cancel(void*ctx) {
    Pipe*p=ctx;
    uint32_t now=pkgi_time_msec();
    if(now-p->last_ui>=100) {
        char extra[128];uint32_t ms=now-p->start;
        unsigned speed=ms?(unsigned)(p->consumed*1000/ms/1024):0;
        snprintf(extra,sizeof(extra),"%u KB/s",speed);
        pkgi_dialog_update_progress(p->name,extra,NULL,p->total?(float)((double)p->consumed/p->total):0);
        p->last_ui=now;
        progress_screen_refresh();
    }
    if(pkgi_exit_requested() || pkgi_dialog_is_cancelled())p->abort=1;
    return p->abort;
}
static int producer_slot(Pipe*p) {
    while(!p->abort) {
        SceUInt timeout=20000;
        if(sceKernelWaitSema(p->free_slots,1,&timeout)==0){__sync_synchronize();return 1;}
    }
    return 0;
}
static void publish(Pipe*p,unsigned size) {
    p->sizes[p->write_slot]=size;
    __sync_synchronize();
    sceKernelSignalSema(p->ready,1);
    p->write_slot=(p->write_slot+1)%SLOTS;p->fill=0;
}
static size_t receive(void*data,size_t size,size_t count,void*ctx) {
    Pipe*p=ctx;if(size && count>SIZE_MAX/size)return 0;
    size_t n=size*count,done=0;
    while(done<n) {
        if(p->abort)return 0;
        if(!p->fill && !producer_slot(p))return 0;
        unsigned take=SLOT_SIZE-p->fill;if(take>n-done)take=n-done;
        memcpy(p->data+p->write_slot*SLOT_SIZE+p->fill,(char*)data+done,take);
        p->fill+=take;done+=take;p->received+=take;
        if(p->fill==SLOT_SIZE)publish(p,SLOT_SIZE);
    }
    return n;
}
/* Native/curl progress callback userdata is NULL in this application's HTTP wrapper. */
static Pipe* active_pipe;
static int network_progress(void*ctx,int64_t a,int64_t b,int64_t c,int64_t d) {
    (void)ctx;(void)a;(void)b;(void)c;(void)d;return active_pipe->abort;
}
static int producer(SceSize args,void*argp) {
    Pipe*p=*(Pipe**)argp;
    /* HTTP wrapper supplies NULL userdata; one active transfer owns the bridge. */
    extern size_t pkgi_stream_receive_bridge(void*,size_t,size_t,void*);
    int ok=pkgi_http_read(p->http,pkgi_stream_receive_bridge,network_progress);
    p->http_elapsed_ms=pkgi_time_msec()-p->start;
    if(!p->abort) {
        if(p->fill)publish(p,p->fill);
        if(producer_slot(p)) {
            p->transport_ok=ok;__sync_synchronize();publish(p,0);
        }
    }
    __sync_synchronize();p->done=1;
    return 0;
}
size_t pkgi_stream_receive_bridge(void*d,size_t s,size_t n,void*ctx) {
    (void)ctx;return receive(d,s,n,active_pipe);
}
static int take_slot(Pipe*p) {
    while(!p->abort) {
        if(ui_cancel(p))break;
        SceUInt timeout=10000;
        if(sceKernelWaitSema(p->ready,1,&timeout)==0) {
            __sync_synchronize();p->held=1;p->read_pos=0;return 1;
        }
        if(p->done) {
            if(sceKernelPollSema(p->ready,1)==0) {
                __sync_synchronize();p->held=1;p->read_pos=0;return 1;
            }
            return 0;
        }
    }
    return 0;
}
static void release_slot(Pipe*p) {
    p->held=0;p->read_slot=(p->read_slot+1)%SLOTS;__sync_synchronize();sceKernelSignalSema(p->free_slots,1);
}
static int read_pipe(void*ctx,void*buf,unsigned n) {
    Pipe*p=ctx;if(ui_cancel(p))return -1;
    if(!p->held && !take_slot(p))return -1;
    unsigned size=p->sizes[p->read_slot];
    if(!size)return 0;
    unsigned take=size-p->read_pos;if(take>n)take=n;
    memcpy(buf,p->data+p->read_slot*SLOT_SIZE+p->read_pos,take);
    p->read_pos+=take;p->consumed+=take;
    if(p->read_pos==size)release_slot(p);
    return take;
}
static int finish_pipe(void*ctx) {
    Pipe*p=ctx;
    if(!p->held && !take_slot(p))return 0;
    int ok=p->sizes[p->read_slot]==0 && p->transport_ok && !ui_cancel(p);
    release_slot(p);return ok;
}
static void progress(void*ctx,uint64_t pos) {(void)pos;ui_cancel(ctx);}
int pkgi_stream_download(const DbItem*item) {
    Pipe p;memset(&p,0,sizeof(p));p.free_slots=p.ready=p.thread=-1;p.name=item->name;
    p.start=p.last_ui=pkgi_time_msec();int result=0,started=0;
    pkgi_stream_source source={.ctx=&p,.read=read_pipe,.finish=finish_pipe,.cancelled=ui_cancel,.progress=progress,.root=pkgi_get_storage_device()};
    pkgi_dialog_start_progress("Downloading + Installing...",item->name,0);
    pkgi_dialog_allow_close(1);
    p.http=pkgi_http_get(item->url,item->content,0);
    if(!p.http){snprintf(source.error,sizeof(source.error),"Could not send HTTP request");goto cleanup;}
    int64_t length;
    if(!pkgi_http_response_length(p.http,&length)||length<=0) {snprintf(source.error,sizeof(source.error),"Streaming requires a known Content-Length");goto cleanup;}
    p.total=source.length=length;
    if(!pkgi_check_free_space(length)){snprintf(source.error,sizeof(source.error),"Not enough free space");goto cleanup;}
    p.data=malloc(SLOT_SIZE*SLOTS);
    p.free_slots=sceKernelCreateSema("stream_free",0,SLOTS,SLOTS,NULL);
    p.ready=sceKernelCreateSema("stream_ready",0,0,SLOTS,NULL);
    if(!p.data || p.free_slots<0 || p.ready<0){snprintf(source.error,sizeof(source.error),"Cannot allocate 8 MiB stream queue");goto cleanup;}
    p.thread=sceKernelCreateThread("stream_http",producer,0x18,0x10000,PSP_THREAD_ATTR_USER,NULL);
    active_pipe=&p;Pipe*arg=&p;
    if(p.thread<0 || sceKernelStartThread(p.thread,sizeof(arg),&arg)<0){snprintf(source.error,sizeof(source.error),"Cannot start HTTP worker");goto cleanup;}
    started=1;
    result=pkgi_stream_unpack(&source);
cleanup:
    p.abort=1;__sync_synchronize();
    if(started) {
        /* Never terminate a thread inside sceHttp/curl; wait for call/timeout and cleanup. */
        while(!p.done){progress_screen_refresh();sceKernelDelayThread(10000);}
        sceKernelWaitThreadEnd(p.thread,NULL);
    }
    if(p.thread>=0)sceKernelDeleteThread(p.thread);
    if(p.http)pkgi_http_close(p.http);
    if(p.ready>=0)sceKernelDeleteSema(p.ready);
    if(p.free_slots>=0)sceKernelDeleteSema(p.free_slots);
    free(p.data);active_pipe=NULL;
    FILE*f=fopen("pkgi-stream-log.txt","w");
    if(f){fprintf(f,"build: %s\nresult: %d\nconsumed: %llu\nreceived: %llu\nexpected: %llu\nelapsed_ms: %u\nmessage: %s\n",PKGI_VERSION,result,(unsigned long long)p.consumed,(unsigned long long)p.received,(unsigned long long)p.total,pkgi_time_msec()-p.start,source.error);
        fprintf(f,"crypto_mode: ME pipeline (CPU fallback if unavailable)\nhttp_elapsed_ms: %u\nme_payload_bytes: %llu\ncpu_payload_bytes: %llu\npayload_written: %llu\nme_payload_jobs: %u\n",p.http_elapsed_ms,(unsigned long long)source.me_bytes,(unsigned long long)source.cpu_bytes,(unsigned long long)source.payload_written,source.me_jobs);
        fclose(f);}
    if(!result && !pkgi_exit_requested() && (!pkgi_dialog_is_cancelled() || strstr(source.error,"Cleanup failed")))pkgi_dialog_error(source.error[0]?source.error:"Streaming installation failed");
    return result;
}
