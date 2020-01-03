#pragma once
#include <windows.h>

struct pipe_io_op {
    //void (*read)(void*, DWORD);
    void (*abort)();
};

void PipeClose();
BOOL PipeConnect();
int PipeSend(const void* pContent, DWORD length);
int PipeReceive(void* pContent,  DWORD length);


void RegisterIO(struct pipe_io_op*);