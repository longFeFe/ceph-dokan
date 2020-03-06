#include "pipe_client.h"
#include "protocol.h"
#include <stdio.h>
#include <windows.h>




struct pipe_io_op* pIOFunction = NULL;
void RegisterIO(struct pipe_io_op* pio) {
	pIOFunction = pio;
}

void* WatchPipeHandle(void* p)
{	
	HANDLE hServer = (HANDLE)p;

	if (hServer == INVALID_HANDLE_VALUE) {
		OutputDebugStringW(L"WatchPipeHandle:HANDLE = INVALID_HANDLE_VALUE\n");
	} else {
		char content[32] = { 0 };
		int errorcode = PipeReceive(hServer, content, 32);
		sprintf(content, "WatchPipeHandle Error:%d\n", errorcode);
		OutputDebugStringA(content);
	}

	if (pIOFunction)
		pIOFunction->abort();
	return NULL;
}

void PipeClose(HANDLE h)
{
	CloseHandle(h);
}

HANDLE PipeConnect(LPCSTR path)
{
	return CreateFileA(
		path, // pipe name
		GENERIC_READ |	 // read and write access
			GENERIC_WRITE,
		0,			   // no sharing
		NULL,		   // default security attributes
		OPEN_EXISTING, // opens existing pipe
		0,			   // default attributes
		NULL);		   // no template file
}

int PipeSend(HANDLE hPipe, const void *pContent, DWORD length)
{

	DWORD cbWritten = 0;
	//send msg
	BOOL fSuccess = WriteFile(
		hPipe,		// pipe handle
		pContent,   // message
		length,		// message length
		&cbWritten, // bytes written
		NULL);		// not overlapped
	//管道裂开
	DWORD nError = GetLastError();
	if (nError == ERROR_MORE_DATA || nError == ERROR_SUCCESS) {
		return cbWritten;
	}
	OutputDebugStringW(L"Send error \n");
	return -nError;
}

int PipeReceive(HANDLE hPipe, void* pContent,  DWORD length)
{
	// if (hPipe == INVALID_HANDLE_VALUE)
	// {
	// 	return 0;
	// }
	ZeroMemory(pContent, length);
	BOOL fSuccess = ReadFile(
		hPipe,	// pipe handle
		pContent, // buffer to receive reply
		length,   // size of buffer
		&length,  // number of bytes read
		NULL);	// not overlapped


	//管道裂开
	DWORD nError = GetLastError();
	if (nError == ERROR_MORE_DATA || nError == ERROR_SUCCESS) {
		if (nError == ERROR_MORE_DATA) OutputDebugStringW(L"Receive error: ERROR_MORE_DATA\n");
		return length;
	}

	OutputDebugStringW(L"Receive error \n");
	return -nError;
}


void ListServerState(HANDLE hServer) {
	DWORD tid;
	CreateThread(NULL, 0, (LPTHREAD_START_ROUTINE)WatchPipeHandle, hServer, 0, &tid);
}