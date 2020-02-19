//调整ceph数据
#ifndef __REVISE__H_
#define __REVISE__H_
#include <Windows.h>
#if defined(__cplusplus) || defined(c_plusplus)
extern "C" {
#endif
    void DdmPrintW(LPCWSTR format, ...);
    void DdmPrintA(LPCSTR format, ...);
    int SendDataToUI2(HANDLE hPipe, int action, void* ctx, DWORD ctx_len);
    int ReadDataFromUI2(HANDLE hPipe, void* receive, DWORD receive_len);
    BOOL IsOfficeTempFile(const char*);
    DWORD FileAuth(const char*);
    DWORD FileParentAuth(const char*);
    BOOL  FileExist(const char*);
    BOOL FileCreate(const char*, BOOL);
    BOOL FileDelete(const char*, BOOL);
    BOOL FileMove(const char*, const char*, BOOL);
#if defined(__cplusplus) || defined(c_plusplus)
}
#endif
#endif