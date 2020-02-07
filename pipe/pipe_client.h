#pragma once
#include <windows.h>

struct pipe_io_op {
    //void (*read)(void*, DWORD);
    void (*abort)();
};

void PipeClose(HANDLE);
HANDLE PipeConnect(LPCSTR);
int PipeSend(HANDLE, const void* pContent, DWORD length);
int PipeReceive(HANDLE, void* pContent,  DWORD length);


void RegisterIO(struct pipe_io_op*);