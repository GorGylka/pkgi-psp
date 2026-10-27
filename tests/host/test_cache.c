#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../../source/pkgi_download.c"
static size_t bytes, calls;
static int fail_write;
int pkgi_write(void* f,const void* data,uint32_t n){(void)f;if(fail_write)return 0;const unsigned char*p=data;for(size_t i=0;i<n;i++)assert(p[i]==(unsigned char)((bytes+i)%251));bytes+=n;calls++;return 1;}
int main(void){
 download_cache=malloc(DOWNLOAD_CACHE_SIZE);assert(download_cache);
 size_t size=2*DOWNLOAD_CACHE_SIZE+12345;unsigned char* data=malloc(size);assert(data);
 for(size_t i=0;i<size;i++)data[i]=i%251;
 assert(write_download_data(data,1,size,NULL)==size);
 assert(bytes==2*DOWNLOAD_CACHE_SIZE);assert(calls==2);assert(cache_used==12345);
 assert(flush_download_cache());assert(bytes==size);assert(calls==3);assert(download_offset==size);
 assert(flush_download_cache());assert(calls==3);
 cache_used=0;bytes=0;calls=0;download_offset=0;fail_write=1;
 assert(write_download_data(data,1,DOWNLOAD_CACHE_SIZE,NULL)==0);assert(download_offset==0);
 assert(!flush_download_cache());cache_used=0;
 assert(write_download_data(data,SIZE_MAX,2,NULL)==0);
 free(data);free(download_cache);puts("8 MiB cache: multi-flush, exact boundary, final partial block, write failure and overflow PASS");
}
