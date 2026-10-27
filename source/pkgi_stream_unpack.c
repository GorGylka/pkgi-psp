#include "pkgi_stream.h"
#include "pkgi_pair_crypto.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdint.h>
#include <sys/stat.h>
#include <unistd.h>
#include <dirent.h>
#include <errno.h>

#define MAX_ITEMS 4096u
#define MAX_PREFIX (4u*1024u*1024u)
#define CHUNK (2u*PKGI_ME_BLOCK_SIZE)
static const uint8_t key[16] = {0x07,0xF2,0xC6,0x82,0x90,0xB5,0x0D,0x2C,0x33,0x81,0x8D,0x70,0x9B,0x60,0xE6,0x2B};
typedef struct { uint64_t offset,size; uint32_t name,len; int directory; char path[241]; } Entry;
static uint32_t be32(const uint8_t*p) { return (uint32_t)p[0]<<24|(uint32_t)p[1]<<16|(uint32_t)p[2]<<8|p[3]; }
static uint64_t be64(const uint8_t*p) {return (uint64_t)be32(p)<<32|be32(p+4);}
static int fail(pkgi_stream_source*s,const char*e) {snprintf(s->error,sizeof(s->error),"%s",e);return 0;}
static int cancelled(pkgi_stream_source*s) {return s->cancelled && s->cancelled(s->ctx);}
static int exact(pkgi_stream_source*s,void*p,unsigned n,uint64_t*pos) {
    unsigned done=0;
    while(done<n) {
        if(cancelled(s)) return fail(s,"Cancelled");
        int r=s->read(s->ctx,(char*)p+done,n-done);
        if(r<=0 || (unsigned)r>n-done) return fail(s,"Stream interrupted or incomplete");
        done+=r;*pos+=r;
        if(s->progress)s->progress(s->ctx,*pos);
    }
    return cancelled(s)?fail(s,"Cancelled"):1;
}
static int skip_to(pkgi_stream_source*s,uint64_t target,uint64_t*pos,uint8_t*b) {
    if(target<*pos || target>s->length)return fail(s,"PKG requires backward access");
    while(*pos<target) {
        unsigned n=target-*pos>CHUNK?CHUNK:(unsigned)(target-*pos);
        if(!exact(s,b,n,pos))return 0;
    }
    return 1;
}
static int safe_path(const uint8_t*p,unsigned n,char*out) {
    const char prefix[]="USRDIR/CONTENT/";
    if(n && p[n-1]==0)--n;
    if(n<=15 || n>255 || memcmp(p,prefix,15))return 0;
    p+=15;n-=15;
    if(n>240 || p[0]=='/' || p[n-1]=='/')return 0;
    unsigned part=0,depth=0;
    for(unsigned i=0;i<=n;i++) {
        if(i==n || p[i]=='/') {
            unsigned size=i-part;
            if(!size || (size==1&&p[part]=='.') || (size==2&&p[part]=='.'&&p[part+1]=='.') || ++depth>16)return 0;
            part=i+1;
        } else if(p[i]<32 || p[i]==127 || p[i]=='\\' || p[i]==':' || p[i]=='*' || p[i]=='?' || p[i]=='"' || p[i]=='<' || p[i]=='>' || p[i]=='|')return 0;
    }
    memcpy(out,p,n);out[n]=0;return 1;
}
static int mkdirs(const char*path) {
    char b[512];if(strlen(path)>=sizeof(b))return 0;strcpy(b,path);
    // ms0:/ and ef0:/ are device roots, not directories to create.
    char*start=b+1;
    char*colon=strchr(b, ':');
    if(colon) {
        if(colon==b || colon[1]!='/')return 0;
        start=colon+2;
    }
    for(char*q=start;;q++)if(*q=='/' || !*q) {
        char c=*q;*q=0;
        struct stat st;
        if(stat(b,&st)<0) {
            if(mkdir(b,0777)<0 && errno!=EEXIST)return 0;
            if(stat(b,&st)<0)return 0;
        }
        if(!S_ISDIR(st.st_mode))return 0;
        *q=c;if(!c)break;
    }
    return 1;
}
static int remove_tree(const char*path) {
    DIR*d=opendir(path);if(!d)return remove(path)==0 || errno==ENOENT;
    int ok=1;struct dirent*e;
    while((e=readdir(d))) {
        if(!strcmp(e->d_name,".")||!strcmp(e->d_name,".."))continue;
        char child[512];int n=snprintf(child,sizeof(child),"%s/%s",path,e->d_name);
        if(n<0 || n>=(int)sizeof(child) || !remove_tree(child))ok=0;
    }
    closedir(d);if(rmdir(path)<0)ok=0;return ok;
}
static int by_offset(const void*a,const void*b) {
    const Entry*x=a,*y=b;return x->offset<y->offset?-1:x->offset>y->offset?1:0;
}
static int by_path(const void*a,const void*b) {return strcasecmp(((const Entry*)a)->path,((const Entry*)b)->path);}

int pkgi_stream_unpack(pkgi_stream_source*s) {
    uint8_t header[256],iv[16];uint8_t*prefix=NULL,*buf=NULL;Entry*entries=NULL;
    uint64_t pos=0;unsigned used=0;int ok=0,me_started=0,use_me=0,stage_created=0;
    unsigned pending_size=0;uint8_t*pending_output=NULL;
    char stage[512]={0},target[512],parent[512],backup[512];FILE*out=NULL;
    mbedtls_aes_context aes;mbedtls_aes_init(&aes);s->error[0]=0;
    s->me_bytes=s->cpu_bytes=s->payload_written=0;s->me_jobs=0;
#define REQUIRE(c,msg) do{if(!(c)){fail(s,msg);goto cleanup;}}while(0)
    REQUIRE(s->length>=256,"PKG is too short");
    if(!exact(s,header,256,&pos))goto cleanup;
    REQUIRE(!memcmp(header,"\x7fPKG\x80\0\0\2",8),"Streaming supports retail PSP PKG only");
    uint64_t enc=be64(header+0x20),encsize=be64(header+0x28);
    uint32_t count=be32(header+0x14),meta=be32(header+8);
    REQUIRE(be64(header+0x18)==s->length,"PKG / HTTP length mismatch");
    REQUIRE(enc>=256 && enc<=MAX_PREFIX && enc<=s->length && encsize<=s->length-enc,"Invalid PKG data range");
    REQUIRE(count && count<=MAX_ITEMS && (uint64_t)count*32<=encsize,"Unsupported PKG item table size");
    REQUIRE(meta>=256 && (uint64_t)meta+0x51<=enc,"Unsupported PKG metadata layout");
    buf=malloc(CHUNK);prefix=malloc((size_t)enc);entries=calloc(count,sizeof(*entries));
    REQUIRE(buf&&prefix&&entries,"Not enough RAM for streaming");
    memcpy(prefix,header,256);
    if(!exact(s,prefix+256,(unsigned)enc-256,&pos))goto cleanup;
    REQUIRE(be32(prefix+meta+0x14)!=9,"Themes require the ordinary installer");
    char gameid[10];memcpy(gameid,prefix+meta+0x48,9);gameid[9]=0;
    for(unsigned i=0;i<9;i++)REQUIRE((gameid[i]>='A'&&gameid[i]<='Z')||(gameid[i]>='0'&&gameid[i]<='9'),"Invalid game ID");
    memcpy(iv,header+0x70,16);
    REQUIRE(!mbedtls_aes_setkey_enc(&aes,key,128),"AES initialization failed");
    free(prefix);prefix=malloc(count*32);
    REQUIRE(prefix,"Not enough RAM for PKG table");
    if(!exact(s,prefix,count*32,&pos))goto cleanup;
    REQUIRE(pkgi_ctr_xor(&aes,iv,0,prefix,count*32),"PKG table decryption failed");
    uint64_t first=encsize,name_end=(uint64_t)count*32;
    for(unsigned i=0;i<count;i++) {
        const uint8_t*t=prefix+i*32;
        if(t[24]!=0x90)continue;
        Entry*e=&entries[used++];e->name=be32(t);e->len=be32(t+4);e->offset=be64(t+8);e->size=be64(t+16);
        e->directory=t[27]==4 || t[27]==18;
        REQUIRE(e->name>=(uint64_t)count*32 && e->len>0 && e->len<=255 && (uint64_t)e->name+e->len<=encsize,"Invalid PKG filename range");
        REQUIRE(e->offset<=encsize && e->size<=encsize-e->offset,"Invalid PKG file range");
        if(!e->directory && e->size) {
            REQUIRE((e->offset&15)==0,"Unaligned file offset unsupported");
            if(e->offset<first)first=e->offset;
        }
        if((uint64_t)e->name+e->len>name_end)name_end=(uint64_t)e->name+e->len;
    }
    REQUIRE(used,"No PSP/GAME files in PKG");
    name_end=(name_end+15)&~(uint64_t)15;
    REQUIRE(name_end<=first && name_end<=MAX_PREFIX && name_end<=encsize,"PKG filenames are not before payload; streaming unsupported");
    uint8_t*grown=realloc(prefix,(size_t)name_end);REQUIRE(grown,"Not enough RAM for filenames");prefix=grown;
    unsigned table=count*32;
    if(!exact(s,prefix+table,(unsigned)name_end-table,&pos))goto cleanup;
    REQUIRE(pkgi_ctr_xor(&aes,iv,table/16,prefix+table,(size_t)name_end-table),"PKG filename decryption failed");
    unsigned valid=0;
    for(unsigned i=0;i<used;i++) {
        Entry*e=&entries[i];const uint8_t*n=prefix+e->name;unsigned len=e->len;
        if(len && n[len-1]==0)--len;
        if(e->directory && ((len==6&&!memcmp(n,"USRDIR",6)) ||
            (len==14&&!memcmp(n,"USRDIR/CONTENT",14)) || (len==15&&!memcmp(n,"USRDIR/CONTENT/",15))))continue;
        if(e->directory && len && n[len-1]=='/')--len;
        REQUIRE(safe_path(n,len,e->path),"Unsafe or unsupported path in PKG");
        if(valid!=i)entries[valid]=*e;
        ++valid;
    }
    used=valid;
    free(prefix);prefix=NULL;
    qsort(entries,used,sizeof(*entries),by_path);
    int has_eboot=0;
    for(unsigned i=0;i<used;i++) {
        if(!entries[i].directory && !strcasecmp(entries[i].path,"EBOOT.PBP") && entries[i].size)has_eboot=1;
        if(i) {
            Entry*a=&entries[i-1],*b=&entries[i];size_t n=strlen(a->path);
            REQUIRE(strcasecmp(a->path,b->path),"Duplicate filename in PKG");
            REQUIRE(a->directory || strncasecmp(a->path,b->path,n) || b->path[n]!='/',"File/directory conflict in PKG");
        }
    }
    REQUIRE(has_eboot,"PKG has no EBOOT.PBP");
    qsort(entries,used,sizeof(*entries),by_offset);
    uint64_t end=name_end;
    for(unsigned i=0;i<used;i++)if(!entries[i].directory&&entries[i].size) {
        REQUIRE(entries[i].offset>=end,"Overlapping files in PKG");end=entries[i].offset+entries[i].size;
    }
    REQUIRE(snprintf(parent,sizeof(parent),"%s/PSP/GAME",s->root)<(int)sizeof(parent) && mkdirs(parent),"Cannot create staging parent");
    static unsigned sequence;
    for(unsigned attempt=0;attempt<1024;attempt++) {
        REQUIRE(snprintf(stage,sizeof(stage),"%s/.pkgi-stream-%08x",parent,++sequence)<(int)sizeof(stage),"Staging path too long");
        if(mkdir(stage,0777)==0){stage_created=1;break;}
        if(errno!=EEXIST)break;
    }
    REQUIRE(stage_created,"Cannot create private staging directory");
    REQUIRE(snprintf(target,sizeof(target),"%s/PSP/GAME/%s",s->root,gameid)<(int)sizeof(target),"Target path too long");
    snprintf(parent,sizeof(parent),"%s/PSP/GAME",s->root);
    REQUIRE(mkdirs(parent),"Cannot create PSP/GAME");
    use_me=pkgi_me_begin(key);me_started=1;
    for(unsigned i=0;i<used;i++) {
        Entry*e=&entries[i];char path[512];
        REQUIRE(!cancelled(s),"Cancelled");
        REQUIRE(snprintf(path,sizeof(path),"%s/%s",stage,e->path)<(int)sizeof(path),"Output path too long");
        if(e->directory){REQUIRE(mkdirs(path),"Cannot create output directory");continue;}
        char*slash=strrchr(path,'/');*slash=0;int dirs=mkdirs(path);*slash='/';REQUIRE(dirs,"Cannot create output parents");
        if(e->size && !skip_to(s,enc+e->offset,&pos,buf))goto cleanup;
        out=fopen(path,"wb");REQUIRE(out,"Cannot open staged file");
        uint64_t done=0;
        if(use_me && e->size) {
            /* Reuse the existing 512 KiB allocation as two 256 KiB buffers.
               ME owns an internal copy, so CPU can read ahead while it runs. */
            uint8_t*current=buf,*next=buf+PKGI_ME_BLOCK_SIZE;
            unsigned n=e->size>PKGI_ME_BLOCK_SIZE?PKGI_ME_BLOCK_SIZE:(unsigned)e->size;
            if(!exact(s,current,n,&pos))goto cleanup;
            REQUIRE(pkgi_me_submit(current,n,iv,e->offset/16),"ME submit failed");
            pending_size=n;pending_output=current;
            while(n) {
                uint64_t left=e->size-done-n;
                unsigned next_n=left>PKGI_ME_BLOCK_SIZE?PKGI_ME_BLOCK_SIZE:(unsigned)left;
                /* Wait for network data without holding up the independent ME. */
                if(next_n && !exact(s,next,next_n,&pos))goto cleanup;
                int joined=pkgi_me_wait(current,n);pending_size=0;
                REQUIRE(joined,"ME decryption failed");
                s->me_bytes+=n;++s->me_jobs;
                REQUIRE(!cancelled(s),"Cancelled");
                /* Start the next job before writing this completed buffer. */
                if(next_n) {
                    REQUIRE(pkgi_me_submit(next,next_n,iv,(e->offset+done+n)/16),"ME submit failed");
                    pending_size=next_n;pending_output=next;
                }
                unsigned written=0;
                while(written<n) {
                    size_t w=fwrite(current+written,1,n-written,out);
                    REQUIRE(w,"Memory Stick write error");written+=w;s->payload_written+=w;
                }
                done+=n;
                uint8_t*swap=current;current=next;next=swap;n=next_n;
            }
        } else while(done<e->size) {
            unsigned n=e->size-done>CHUNK?CHUNK:(unsigned)(e->size-done);
            if(!exact(s,buf,n,&pos))goto cleanup;
            REQUIRE(pkgi_ctr_xor(&aes,iv,(e->offset+done)/16,buf,n),"CPU decryption failed");
            s->cpu_bytes+=n;
            REQUIRE(!cancelled(s),"Cancelled");
            unsigned written=0;
            while(written<n) {size_t w=fwrite(buf+written,1,n-written,out);REQUIRE(w,"Memory Stick write error");written+=w;s->payload_written+=w;}
            done+=n;
        }
        int rc=fclose(out);out=NULL;REQUIRE(!rc,"Memory Stick flush error");
    }
    /* Receive all padding/trailer and require successful transport EOF before publishing. */
    if(!skip_to(s,s->length,&pos,buf))goto cleanup;
    REQUIRE(!cancelled(s),"Cancelled");
    REQUIRE(s->finish(s->ctx),"HTTP transfer did not finish successfully");
    pkgi_me_end();me_started=0;
    REQUIRE(!cancelled(s),"Cancelled");
    REQUIRE(snprintf(backup,sizeof(backup),"%s.old",stage)<(int)sizeof(backup),"Backup path too long");
    struct stat st;
    REQUIRE(stat(backup,&st)<0 && errno==ENOENT,"Backup path already exists");
    int existed=stat(target,&st)==0;
    if(existed)REQUIRE(rename(target,backup)==0,"Cannot preserve existing installation");
    if(rename(stage,target)<0) {
        if(existed && rename(backup,target)<0)fail(s,"Commit failed; old game preserved in /PSP/GAME/.pkgi-stream-*.old");
        else fail(s,"Cannot publish staged installation");
        goto cleanup;
    }
    stage_created=0;ok=1;
    if(existed && !remove_tree(backup))snprintf(s->error,sizeof(s->error),"Installed; old backup retained in /PSP/GAME/.pkgi-stream-*.old");
cleanup:
    /* Drain any submitted job before stopping ME or releasing its output buffer. */
    if(pending_size) {pkgi_me_wait(pending_output,pending_size);pending_size=0;}
    if(out)fclose(out);
    if(me_started)pkgi_me_end();
    if(stage_created && !remove_tree(stage))fail(s,"Cleanup failed: remove /PSP/GAME/.pkgi-stream-* manually");
    free(prefix);free(entries);free(buf);mbedtls_aes_free(&aes);
    return ok;
#undef REQUIRE
}
