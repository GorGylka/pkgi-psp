#pragma once
#include "../../me/stubs/pspkernel.h"
typedef unsigned SceUInt;
#define PSP_THREAD_ATTR_USER 0
int sceKernelWaitSema(int,int,SceUInt*);
int sceKernelPollSema(int,int);
int sceKernelSignalSema(int,int);
int sceKernelCreateSema(const char*,int,int,int,void*);
int sceKernelDeleteSema(int);
int sceKernelCreateThread(const char*,int(*)(SceSize,void*),int,int,int,void*);
int sceKernelStartThread(int,unsigned,void*);
int sceKernelWaitThreadEnd(int,SceUInt*);
int sceKernelDeleteThread(int);
int sceKernelDelayThread(unsigned);
