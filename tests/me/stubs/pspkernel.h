#pragma once
#include <stdint.h>
#include <fcntl.h>
typedef uint8_t u8;
typedef uint32_t u32;
typedef int SceUID;
typedef uint32_t SceSize;
typedef int64_t SceOff;
#define PSP_O_RDONLY O_RDONLY
#define PSP_O_WRONLY O_WRONLY
#define PSP_O_CREAT O_CREAT
#define PSP_O_TRUNC O_TRUNC
#define PSP_SEEK_SET 0
#define PSP_SEEK_END 2
int sceIoOpen(const char*,int,int);
int sceIoClose(int);
int sceIoRead(int,void*,unsigned);
int sceIoWrite(int,const void*,unsigned);
SceOff sceIoLseek(int,SceOff,int);
int sceIoRemove(const char*);
