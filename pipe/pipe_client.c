#include "pipe_client.h"
#include "protocol.h"
#include <stdio.h>
#include <windows.h>

HANDLE hPipe = INVALID_HANDLE_VALUE;
HANDLE hWatchThread = INVALID_HANDLE_VALUE;
void *WatchPipeHandle()
{	DWORD iRet = 0;
	DWORD iError = 0;
	// do
	// {
	// 	iRet = WaitForSingleObject(hPipe, 5 * 1000);
	// 	iError = GetLastError();
	// 	char error[1024] = {0};
	// 	sprintf(error, "WaitForSingleObject Return = %d, Lasterror = %d\n", iRet, iError);
	// 	OutputDebugStringA(error);
	// } while (iRet == WAIT_TIMEOUT || iError == 0);
	
	
	//TODO lisenting server close
	//Unmount  exit(0);
	return NULL;
}

void PipeClose()
{
	CloseHandle(hPipe);
	TerminateThread(hWatchThread, 0);
	CloseHandle(hWatchThread);
	hPipe = INVALID_HANDLE_VALUE;
	hWatchThread = INVALID_HANDLE_VALUE;
}

BOOL PipeConnect()
{
	hPipe = CreateFileA(
		CEPH_CHANNEL_NAME, // pipe name
		GENERIC_READ |	 // read and write access
			GENERIC_WRITE,
		0,			   // no sharing
		NULL,		   // default security attributes
		OPEN_EXISTING, // opens existing pipe
		0,			   // default attributes
		NULL);		   // no template file

	if (hPipe == INVALID_HANDLE_VALUE)
	{
		char error[1024];
		sprintf(error, "connect error = %d\n", GetLastError());
		OutputDebugStringA(error);
		return FALSE;
	}
	DWORD tid;
	hWatchThread = CreateThread(NULL, 0, (LPTHREAD_START_ROUTINE)WatchPipeHandle, NULL, 0, &tid);
	return TRUE;
}

int PipeSend(const void *pContent, DWORD length)
{

	if (hPipe == INVALID_HANDLE_VALUE)
	{
		return 0;
	}
	DWORD cbWritten = 0;
	//send msg
	BOOL fSuccess = WriteFile(
		hPipe,		// pipe handle
		pContent,   // message
		length,		// message length
		&cbWritten, // bytes written
		NULL);		// not overlapped

	return cbWritten;
}

int PipeReceive(void *pContent, DWORD length)
{
	if (hPipe == INVALID_HANDLE_VALUE)
	{
		return 0;
	}
	ZeroMemory(pContent, length);
	BOOL fSuccess = ReadFile(
		hPipe,	// pipe handle
		pContent, // buffer to receive reply
		length,   // size of buffer
		&length,  // number of bytes read
		NULL);	// not overlapped

	if (GetLastError() == ERROR_MORE_DATA)
	{
		OutputDebugStringA("Error:ERROR_MORE_DATA\n");
	}
	return length;
}
