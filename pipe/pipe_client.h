#ifndef __PIPE_CLIENT_H_
#define __PIPE_CLIENT_H_
#include <windows.h>
#if defined(__cplusplus) || defined(c_plusplus)
extern "C" {
#endif
struct pipe_io_op {
    //void (*read)(void*, DWORD);
    void (*abort)();
};

void PipeClose(HANDLE);
HANDLE PipeConnect(LPCSTR);
int PipeSend(HANDLE, const void* pContent, DWORD length);
int PipeReceive(HANDLE, void* pContent,  DWORD length);


void RegisterIO(struct pipe_io_op*);
#if defined(__cplusplus) || defined(c_plusplus)
}
#endif
#endif