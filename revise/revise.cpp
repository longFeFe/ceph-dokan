#include "revise.h"
#include "protocol.h"
#include "pipe_client.h"
#include <stdio.h>
#include <iostream>
//#include <algorithm>
using namespace std;

#define MAX_PATH_CEPH 512
void DdmPrintW(LPCWSTR format, ...) {
    WCHAR buffer[512];
    va_list argp;
    va_start(argp, format);
    vswprintf(buffer, format, argp);
    va_end(argp);
    OutputDebugStringW(buffer);
    
}

void DdmPrintA(LPCSTR format, ...) {
    CHAR buffer[512];
    va_list argp;
    va_start(argp, format);
    vsprintf(buffer, format, argp);
    va_end(argp);
    OutputDebugStringA(buffer);
    
}



int ReadDataFromUI2(HANDLE hPipe, void* receive, DWORD receive_len) {
    //FIXME: ERROR_MORE_DATA 
    memset(receive, 0, receive_len);
    return PipeReceive(hPipe, receive, receive_len);
}

int SendDataToUI2(HANDLE hPipe, int action, void* ctx, DWORD ctx_len) {
    pDdm_message pMsg = (pDdm_message)malloc(sizeof(ddm_message) + ctx_len);
    ZeroMemory(pMsg, sizeof(ddm_message) + ctx_len);
    pMsg->action = action;
    pMsg->pid = 0;
    pMsg->ctxLength = ctx_len;
    memcpy(pMsg->ctx, ctx, ctx_len);
    int send_len = PipeSend(hPipe, (void*)pMsg, sizeof(ddm_message) + ctx_len);
    free(pMsg);

    return (send_len - sizeof(ddm_message));
}


//windows路径格式
BOOL IsOfficeTempFile(const char* ptr_name) {
    size_t index_point = string::npos;
    string filename(ptr_name);
    //std::replace(filename.begin(), filename.end(), "\\", "/");
    index_point = filename.rfind("\\");
    if (index_point != string::npos) {
        filename = filename.substr(index_point + 1);
    }
    //1 特殊开头
    if (filename.find("~$") == 0) {
        return TRUE;
    }
    //2 特殊结尾
    index_point = filename.rfind(".");
    if ( index_point != string::npos) {
        string suffix = filename.substr(index_point + 1);
        if (suffix == "tmp")    return TRUE;
    }
    return FALSE;
}


//文件权限
DWORD FileAuth(LPCSTR FileName) {
    HANDLE h = PipeConnect(CEPH_CHANNEL_NAME);

    unsigned short auth = AUTH_ROOT;
    //查询父目录权限
    ctx_common request;
    memset(&request, 0, sizeof(request));
    strcpy(request.path, FileName);
    //request.isDirectory = DokanFileInfo->IsDirectory;
    if (SendDataToUI2(h, DDM_READFILEINFO, (void*)&request, sizeof(request)) !=  sizeof(request)) {
        DdmPrintW(L"Create Failed, PipeSend Error\n");
        return 0;
    }


    pDdm_msg_ret pResult = (pDdm_msg_ret)malloc(sizeof(ddm_msg_ret) + sizeof(ddm_fileInfo));
    int iRecv = ReadDataFromUI2(h, (void*)pResult, sizeof(ddm_msg_ret) + sizeof(ddm_fileInfo));

    
    if (pResult->result == RET_PERSONAL || pResult->result == RET_FAILED) {
        //个人区 或者 节点不存在
    } else {
        pDdm_fileInfo pctx = (pDdm_fileInfo)pResult->ctx;
        //DdmPrintW(L"pctx->mode = %d\n", pctx->mode);
        auth = pctx->mode;
    }
    DdmPrintA("FileAuth %s : %d\n", FileName, auth);
    free(pResult);
    CloseHandle(h);
    return auth;
}


DWORD FileParentAuth(LPCSTR FileName) {
    unsigned short auth = 0;
    string filepath = FileName;
    size_t index = filepath.rfind("\\");
    if (index == string::npos) {
        return auth;
    }
    string fileparent = filepath.substr(0, index);
    return FileAuth(fileparent.c_str());
}


BOOL FileExist(const char* FileName) {
    return FALSE;
}


BOOL FileCreate(const char* FileName) {
    if (IsOfficeTempFile(FileName)) {
        return TRUE;
    }
    DWORD auth = FileParentAuth(FileName);
    if (!(auth & AUTH_CREATE)) {
        DdmPrintA("CreateFile %s ERROR_ACCESS_DENIED\n", FileName);
        return FALSE;
    }
    return TRUE;
}

BOOL FileDelete(const char* FileName) {
    if (IsOfficeTempFile(FileName)) {
        return TRUE;
    }
    DWORD auth = FileAuth(FileName);
    if (!(auth & AUTH_DELETE)) {
        DdmPrintA("Delete %s ERROR_ACCESS_DENIED\n", FileName);
        return FALSE;
    }
    return TRUE;
}

BOOL FileMove(const char* path1, const char* path2) {
    string str_path1 = path1;
    string str_path2 = path2;
    size_t index1 = str_path1.rfind("\\");
    size_t index2 = str_path2.rfind("\\");  
    if (index1 != string::npos && index2 != string::npos &&
        str_path1.substr(0, index1) == str_path1.substr(0, index2)) {
        DWORD auth =  FileAuth(path1);
        if (!(auth == (unsigned short)AUTH_ROOT)) {
            DdmPrintA("Rename %s : ERROR_ACCESS_DENIED\n", path1);
            return FALSE;
        }
    } else {
        //同分区剪贴
        ctx_move ctx;
        memset(&ctx, 0, sizeof(ctx));
      
        strncpy(ctx.pathOld, path1, sizeof(ctx.pathOld));
        strncpy(ctx.pathNew, path2, sizeof(ctx.pathNew));

        HANDLE h =  PipeConnect(CEPH_CHANNEL_NAME);
        if (SendDataToUI2(h, DDM_MOVE , (void*)&ctx, sizeof(ctx)) != sizeof(ctx)) {
            DdmPrintW(L"MoveFile SendDataToUI2 Error\n");
            return FALSE;
        }

        pDdm_msg_ret pResult = (pDdm_msg_ret)malloc(sizeof(ddm_msg_ret));
        ReadDataFromUI2(h, (void*)pResult, sizeof(ddm_msg_ret));
        CloseHandle(h);
        int ret = pResult->result;
        free(pResult);
        if (ret == RET_FORBID) {
            DdmPrintW(L"MoveFile  ERROR_ACCESS_DENIED\n");
            return FALSE;
        }
    }
    return TRUE;
}