#pragma once
#include <windows.h>


void PipeClose();
BOOL PipeConnect();
int PipeSend(const void* pContent, DWORD length);
int PipeReceive(void* pContent,  DWORD length);


