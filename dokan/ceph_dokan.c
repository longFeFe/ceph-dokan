/*
  A CephFS Client on Win32 (based on Dokan)
*/

#define UNICODE
#define _UNICODE
#include <windows.h>
#include <winbase.h>
#include <io.h>
#include <stdio.h>
#include <stdlib.h>
#include "dokan.h"
#include "fileinfo.h"
#include "libcephfs.h"
#include "posix_acl.h"



#include <fcntl.h>
#include <signal.h>
#include <sddl.h>

#include <accctrl.h>
#include <aclapi.h>


#include "pipe_client.h"
#include "protocol.h"
#include "revise.h"
#include "crypto_vrv.h"
#define MAX_PATH_CEPH 8192
#define CEPH_DOKAN_IO_TIMEOUT 1000 * 60 * 2

BOOL WINAPI CCHandler(DWORD);

static BOOL g_UseStdErr;
static BOOL g_DebugMode;
int g_UID = 0;
int g_GID = 0;
BOOL g_UseACL  = FALSE;
struct ceph_mount_info *cmount;


#include <dirent.h>

#define __dev_t unsigned long long
#define __ino_t unsigned long long
#define __nlink_t unsigned long long
#define __mode_t unsigned int
#define __uid_t unsigned int
#define __gid_t unsigned int
#define __off_t long long
#define __blksize_t long long
#define __blkcnt_t long long


struct statvfs
{
    unsigned long int f_bsize;
    unsigned long int f_frsize;

    unsigned long long f_blocks;
    unsigned long long f_bfree;
    unsigned long long f_bavail;
    unsigned long long f_files;
    unsigned long long f_ffree;
    unsigned long long f_favail;

    unsigned long int f_fsid;
#ifdef _STATVFSBUF_F_UNUSED
    int __f_unused;
#endif
    unsigned long int f_flag;
    unsigned long int f_namemax;
    int __f_spare[6];
};

struct fd_context{
    int   fd;
    short delete_on_close;
    short read_only;
};

void UnixTimeToFileTime(time_t t, LPFILETIME pft)
{
    // Note that LONGLONG is a 64-bit value
    LONGLONG ll;
    
    ll = Int32x32To64(t, 10000000) + 116444736000000000;
    pft->dwLowDateTime = (DWORD)ll;
    pft->dwHighDateTime = ll >> 32;
}

void FileTimeToUnixTime(FILETIME ft, time_t *t) 
{ 
    LONGLONG ll; 
    
    ULARGE_INTEGER ui; 
    ui.LowPart  = ft.dwLowDateTime; 
    ui.HighPart = ft.dwHighDateTime; 
    
    ll = ft.dwHighDateTime << 32 + ft.dwLowDateTime; 
    
    *t = ((LONGLONG)(ui.QuadPart - 116444736000000000) / 10000000); 
}

int wchar_to_char(char *strGBK, LPCWSTR FileName, int strlen)
{
    int len = WideCharToMultiByte(CP_UTF8, 0, FileName, -1, NULL, 0, NULL, NULL);
    if(len > strlen){
        return -1;
    }
    WideCharToMultiByte(CP_UTF8, 0, FileName, -1, strGBK, len, NULL, NULL);

    return;
}

int char_to_wchar(LPCWSTR FileName, char *strUtf8, int strlen)
{
    int len = MultiByteToWideChar(CP_UTF8, 0, (LPCTSTR)strUtf8, -1, NULL, 0);
    if(len > strlen){
        return -1;
    }
    MultiByteToWideChar(CP_UTF8, 0, (LPCTSTR)strUtf8, -1, FileName, len);
       
    return 0;
}

static void DbgPrintW(LPCWSTR format, ...)
{
    if (g_DebugMode) {
        WCHAR buffer[512];
        va_list argp;
        va_start(argp, format);
        //vswprintf_s(buffer, sizeof(buffer)/sizeof(WCHAR), format, argp);
        vswprintf(buffer, format, argp);
        va_end(argp);
        if (g_UseStdErr) {
            fwprintf(stderr, buffer);
        } else {
            OutputDebugStringW(buffer);
        }
    }
}

static void AlwaysPrintW(LPCWSTR format, ...)
{
    WCHAR buffer[512];
    va_list argp;
    va_start(argp, format);
    //vswprintf_s(buffer, sizeof(buffer)/sizeof(WCHAR), format, argp);
    vswprintf(buffer, format, argp);
    va_end(argp);
    fwprintf(stderr, buffer);
}

static void DbgPrint(char* format, ...)
{
    if (g_DebugMode) {
        char buffer[512];
        va_list argp;
        va_start(argp, format);
        vsprintf(buffer, format, argp);
        va_end(argp);
        if (g_UseStdErr) {
            fprintf(stderr, buffer);
        } else {
            OutputDebugString(buffer);
        }
    }
}





static WCHAR MountPoint[MAX_PATH_CEPH] = L"M:";
static CHAR LetterName[MAX_PATH] = "vvdisk";
static char ceph_conf_file[MAX_PATH_CEPH];
static WCHAR Wceph_conf_file[MAX_PATH_CEPH];
static WCHAR Wargv0[MAX_PATH_CEPH];

static void
GetFilePath(
    PWCHAR    filePath,
    ULONG    numberOfElements,
    LPCWSTR FileName)
{
    RtlZeroMemory(filePath, numberOfElements * sizeof(WCHAR));
    wcsncat(filePath, FileName, wcslen(FileName));
}

static void GetAbsPath(PWCHAR    filePath, ULONG    numberOfElements, LPCWSTR FileName) {
    RtlZeroMemory(filePath, numberOfElements * sizeof(WCHAR));
    wcsncat(filePath, MountPoint, wcslen(MountPoint));
    wcsncat(filePath, FileName, wcslen(FileName));
}


static void ToLinuxFilePath(char* filePath)
{
    int i;
    for(i = 0; i<strlen(filePath); i++) {
        if( filePath[i] == '\\' ) filePath[i] = '/';
    }
}


/*************************************************************************************/


static void ServerAbort() {
    DdmPrintW(L"Server Pipe abort...");
    DokanRemoveMountPoint(MountPoint);
    exit(0);
}


/**************************************************************************************/
#define WinCephCheckFlag(val, flag) if (val&flag) { DbgPrintW(L"\t" #flag L"\n"); }
#define AlwaysCheckFlag(val, flag) if (val&flag) { AlwaysPrintW(L"\t" #flag L"\n"); }

static int
WinCephCreateFile(
    LPCWSTR                  FileName,
    DWORD                    AccessMode,
    DWORD                    ShareMode,
    DWORD                    CreationDisposition,
    DWORD                    FlagsAndAttributes,
    PDOKAN_FILE_INFO         DokanFileInfo)
{
    WCHAR filePath[MAX_PATH_CEPH];
    GetFilePath(filePath, MAX_PATH_CEPH, FileName);

    //DdmPrintW(L"CreateFile : %s\n", filePath);
    //PrintUserName(DokanFileInfo);

    if (CreationDisposition == CREATE_NEW)
        DbgPrintW(L"\tCREATE_NEW\n");
    if (CreationDisposition == OPEN_ALWAYS)
        DbgPrintW(L"\tOPEN_ALWAYS\n");
    if (CreationDisposition == CREATE_ALWAYS)
        DbgPrintW(L"\tCREATE_ALWAYS\n");
    if (CreationDisposition == OPEN_EXISTING)
        DbgPrintW(L"\tOPEN_EXISTING\n");
    if (CreationDisposition == TRUNCATE_EXISTING)
        DbgPrintW(L"\tTRUNCATE_EXISTING\n");

    if (ShareMode == 0 && AccessMode & FILE_WRITE_DATA)
        ShareMode = FILE_SHARE_WRITE;
    else if (ShareMode == 0)
        ShareMode = FILE_SHARE_READ;

    DbgPrintW(L"\tShareMode = 0x%x\n", ShareMode);

    WinCephCheckFlag(ShareMode, FILE_SHARE_READ);
    WinCephCheckFlag(ShareMode, FILE_SHARE_WRITE);
    WinCephCheckFlag(ShareMode, FILE_SHARE_DELETE);

    DbgPrintW(L"\tAccessMode = 0x%x\n", AccessMode);

    WinCephCheckFlag(AccessMode, GENERIC_READ);
    WinCephCheckFlag(AccessMode, GENERIC_WRITE);
    WinCephCheckFlag(AccessMode, GENERIC_EXECUTE);
    
    WinCephCheckFlag(AccessMode, DELETE);
    WinCephCheckFlag(AccessMode, FILE_READ_DATA);
    WinCephCheckFlag(AccessMode, FILE_READ_ATTRIBUTES);
    WinCephCheckFlag(AccessMode, FILE_READ_EA);
    WinCephCheckFlag(AccessMode, READ_CONTROL);
    WinCephCheckFlag(AccessMode, FILE_WRITE_DATA);
    WinCephCheckFlag(AccessMode, FILE_WRITE_ATTRIBUTES);
    WinCephCheckFlag(AccessMode, FILE_WRITE_EA);
    WinCephCheckFlag(AccessMode, FILE_APPEND_DATA);
    WinCephCheckFlag(AccessMode, WRITE_DAC);
    WinCephCheckFlag(AccessMode, WRITE_OWNER);
    WinCephCheckFlag(AccessMode, SYNCHRONIZE);
    WinCephCheckFlag(AccessMode, FILE_EXECUTE);
    WinCephCheckFlag(AccessMode, STANDARD_RIGHTS_READ);
    WinCephCheckFlag(AccessMode, STANDARD_RIGHTS_WRITE);
    WinCephCheckFlag(AccessMode, STANDARD_RIGHTS_EXECUTE);

//    // When filePath is a directory, needs to change the flag so that the file can be opened.
//    fileAttr = GetFileAttributes(filePath);
//    if (fileAttr && fileAttr & FILE_ATTRIBUTE_DIRECTORY) {
//        FlagsAndAttributes |= FILE_FLAG_BACKUP_SEMANTICS;
//        //AccessMode = 0;
//    }
    DbgPrintW(L"\tFlagsAndAttributes = 0x%x\n", FlagsAndAttributes);

    WinCephCheckFlag(FlagsAndAttributes, FILE_ATTRIBUTE_ARCHIVE);
    WinCephCheckFlag(FlagsAndAttributes, FILE_ATTRIBUTE_ENCRYPTED);
    WinCephCheckFlag(FlagsAndAttributes, FILE_ATTRIBUTE_HIDDEN);
    WinCephCheckFlag(FlagsAndAttributes, FILE_ATTRIBUTE_NORMAL);
    WinCephCheckFlag(FlagsAndAttributes, FILE_ATTRIBUTE_NOT_CONTENT_INDEXED);
    WinCephCheckFlag(FlagsAndAttributes, FILE_ATTRIBUTE_OFFLINE);
    WinCephCheckFlag(FlagsAndAttributes, FILE_ATTRIBUTE_READONLY);
    WinCephCheckFlag(FlagsAndAttributes, FILE_ATTRIBUTE_SYSTEM);
    WinCephCheckFlag(FlagsAndAttributes, FILE_ATTRIBUTE_TEMPORARY);
    WinCephCheckFlag(FlagsAndAttributes, FILE_FLAG_WRITE_THROUGH);
    WinCephCheckFlag(FlagsAndAttributes, FILE_FLAG_OVERLAPPED);
    WinCephCheckFlag(FlagsAndAttributes, FILE_FLAG_NO_BUFFERING);
    WinCephCheckFlag(FlagsAndAttributes, FILE_FLAG_RANDOM_ACCESS);
    WinCephCheckFlag(FlagsAndAttributes, FILE_FLAG_SEQUENTIAL_SCAN);
    WinCephCheckFlag(FlagsAndAttributes, FILE_FLAG_DELETE_ON_CLOSE);
    WinCephCheckFlag(FlagsAndAttributes, FILE_FLAG_BACKUP_SEMANTICS);
    WinCephCheckFlag(FlagsAndAttributes, FILE_FLAG_POSIX_SEMANTICS);
    WinCephCheckFlag(FlagsAndAttributes, FILE_FLAG_OPEN_REPARSE_POINT);
    WinCephCheckFlag(FlagsAndAttributes, FILE_FLAG_OPEN_NO_RECALL);
    WinCephCheckFlag(FlagsAndAttributes, SECURITY_ANONYMOUS);
    WinCephCheckFlag(FlagsAndAttributes, SECURITY_IDENTIFICATION);
    WinCephCheckFlag(FlagsAndAttributes, SECURITY_IMPERSONATION);
    WinCephCheckFlag(FlagsAndAttributes, SECURITY_DELEGATION);
    WinCephCheckFlag(FlagsAndAttributes, SECURITY_CONTEXT_TRACKING);
    WinCephCheckFlag(FlagsAndAttributes, SECURITY_EFFECTIVE_ONLY);
    WinCephCheckFlag(FlagsAndAttributes, SECURITY_SQOS_PRESENT);

    char file_name[MAX_PATH_CEPH];
    int len = wchar_to_char(file_name, FileName, MAX_PATH_CEPH);
    ToLinuxFilePath(file_name);

    struct fd_context fdc;
    memset(&fdc, 0, sizeof(struct fd_context));
    
    //fwprintf(stderr, L"CreateFile ceph_open [%s]\n", FileName);
    //AlwaysCheckFlag(FlagsAndAttributes, FILE_ATTRIBUTE_TEMPORARY);
    //AlwaysCheckFlag(FlagsAndAttributes, FILE_FLAG_DELETE_ON_CLOSE);
    
    if(FlagsAndAttributes&FILE_ATTRIBUTE_TEMPORARY)
    {
        //fwprintf(stderr, L"CreateFile ceph_open FILE_ATTRIBUTE_TEMPORARY[%s]\n", FileName);
        fdc.delete_on_close = TRUE;
        DdmPrintW(L"DELETE_ON_CLOSE1 : %s\n", FileName);
    }
    if(FlagsAndAttributes&FILE_FLAG_DELETE_ON_CLOSE)
    {
        //fwprintf(stderr, L"CreateFile ceph_open FILE_FLAG_DELETE_ON_CLOSE[%s]\n", FileName);
        fdc.delete_on_close = TRUE;
        DdmPrintW(L"DELETE_ON_CLOSE2 : %s\n", FileName);
    }

    DokanResetTimeout(CEPH_DOKAN_IO_TIMEOUT, DokanFileInfo);
    
    int return_value = 0, flags = 0;
    int fd = 0;
    if(strcmp(file_name, "/")==0)
    {
        return 0;
    }
    else
    { 
        // int ret = 0;
        // { //查询文件是否存在
        //     WCHAR absPath[MAX_PATH_CEPH] = { 0 };
        //     GetAbsPath(absPath, MAX_PATH_CEPH, FileName);
        //     char str_path[MAX_PATH_CEPH] = { 0 };
        //     wchar_to_char(str_path, absPath, MAX_PATH_CEPH);
        //     ret = UI2TellMeFileExistWhere(str_path);
        //     if (ret == 2) {      
        //         struct stat st_buf;
        //         ret = ceph_stat(cmount, file_name, &st_buf);
        //     }
        // }
        struct stat st_buf;
        int ret = ceph_stat(cmount, file_name, &st_buf);
       
       // DdmPrintW(L" Create %s, pid = %d\n", FileName, DokanFileInfo->ProcessId);
        if(ret==0) /*File Exists*/
        {
            //struct stat st_buf;
            //ceph_stat(cmount, file_name, &st_buf);
            if(S_ISREG(st_buf.st_mode))
            {                      
                switch (CreationDisposition) {
                    case CREATE_NEW: //文件已存在但是新建
                        return -ERROR_FILE_EXISTS;
                    case TRUNCATE_EXISTING:
                        DdmPrintW(L"CreateFile %s TRUNCATE_EXISTING\n", FileName);
                        //open O_TRUNC & return 0
                        if(g_UseACL)
                        {
                            /* permission check*/
                            int st = permission_walk(cmount, file_name, g_UID, g_GID,
                                                          PERM_WALK_CHECK_WRITE);
                            if(st)
                                return -ERROR_ACCESS_DENIED;
                        }
                        fd = ceph_open(cmount, file_name, O_CREAT|O_TRUNC|O_RDWR, 0755);
                        if(fd<0){
                            DbgPrint("\terror code = %d\n\n", fd);
                            fwprintf(stderr, L"CreateFile REG TRUNCATE_EXISTING ceph_open error [%s][ret=%d]\n", FileName, fd);
                            return fd;
                        }
                        
                        fdc.fd = fd;
                        memcpy(&(DokanFileInfo->Context), &fdc, sizeof(fdc));
                        //fwprintf(stderr, L"CreateFile REG TRUNCATE_EXISTING ceph_open OK [%s][fd=%d][Context=%d]\n", FileName, fd,
                        //    (int)DokanFileInfo->Context);
                        
                        return 0;
                    case OPEN_ALWAYS:
                        DdmPrintW(L"CreateFile %s OPEN_ALWAYS\n", FileName);
                        //open & return ERROR_ALREADY_EXISTS
                        if(AccessMode & GENERIC_READ
                            || AccessMode & FILE_SHARE_READ
                            || AccessMode & STANDARD_RIGHTS_READ
                            || AccessMode & FILE_SHARE_READ)
                        {
                            if(g_UseACL)
                            {
                                /* permission check*/
                                int st = permission_walk(cmount, file_name, g_UID, g_GID,
                                                              PERM_WALK_CHECK_READ);
                                if(st)
                                    return -ERROR_ACCESS_DENIED;
                            }
                        }
                        
                        if(AccessMode & GENERIC_WRITE
                            || AccessMode & FILE_SHARE_WRITE
                            || AccessMode & STANDARD_RIGHTS_WRITE
                            || AccessMode & FILE_SHARE_DELETE)
                        {
                             
                            // if(g_UseACL)
                            // {
                            //     /* permission check*/
                            //     int st = permission_walk(cmount, file_name, g_UID, g_GID,
                            //                                   PERM_WALK_CHECK_WRITE);
                            //     if(st) fdc.read_only = 1;
                            // }
                            //查询文件权限
                            WCHAR absPath[MAX_PATH_CEPH];
                            GetAbsPath(absPath, MAX_PATH_CEPH, FileName);
                            DWORD attrFlag =  GetFileAttributesW(absPath);
                            if (attrFlag & FILE_ATTRIBUTE_READONLY) {
                                DdmPrintW(L"readonly open mode: %s\n", absPath);
                                HANDLE  handle = CreateFileW(L"readonly.txt", AccessMode,//GENERIC_READ|GENERIC_WRITE|GENERIC_EXECUTE,
                                        ShareMode,
                                        NULL, // security attribute
                                        CreationDisposition,
                                        FlagsAndAttributes,// |FILE_FLAG_NO_BUFFERING,
                                        NULL); // template file handle

                                if (handle == INVALID_HANDLE_VALUE) {
                                    DWORD error = GetLastError();
                                    DdmPrintW(L"CreateFileW error code = %d\n", error);
                                    return error * -1; // error codes are negated value of Windows System Error codes
                                }
                                CloseHandle(handle);
                                
                                fdc.read_only = 1;
                            }

                        }
                        
                        if(fdc.read_only == 1)
                            fd = ceph_open(cmount, file_name, O_RDONLY, 0755);
                        else
                            fd = ceph_open(cmount, file_name, O_RDWR, 0755);
                        if(fd<0){
                            DbgPrint("\terror code = %d\n", fd);
                            fwprintf(stderr, L"CreateFile REG OPEN_ALWAYS ceph_open error [%s][ret=%d]\n", FileName, fd);
                            return fd;
                        }
                        
                        fdc.fd = fd;
                        memcpy(&(DokanFileInfo->Context), &fdc, sizeof(fdc));
                        //fwprintf(stderr, L"CreateFile ceph_open REG OPEN_ALWAYS OK [%s][fd=%d][Context=%d]\n", FileName, fd,
                        //    (int)DokanFileInfo->Context);
                        
                        return ERROR_ALREADY_EXISTS;
                    case OPEN_EXISTING:
                        DdmPrintW(L"CreateFile %s OPEN_EXISTING\n", FileName);
                        //open & return 0
                        if(AccessMode & GENERIC_READ
                            || AccessMode & FILE_SHARE_READ
                            || AccessMode & STANDARD_RIGHTS_READ
                            || AccessMode & FILE_SHARE_READ)
                        {
                            //fwprintf(stderr, L"CreateFile REG OPEN_EXISTING ceph_open ACL READ [%s]\n", FileName);
                            if(g_UseACL)
                            {
                                /* permission check*/
                                int st = permission_walk(cmount, file_name, g_UID, g_GID,
                                                              PERM_WALK_CHECK_READ);
                                if(st)
                                    return -ERROR_ACCESS_DENIED;
                            }
                        }
                        
                        if(AccessMode & GENERIC_WRITE
                            || AccessMode & FILE_SHARE_WRITE
                            || AccessMode & STANDARD_RIGHTS_WRITE
                            || AccessMode & FILE_SHARE_DELETE)
                        {
                            // if(g_UseACL)
                            // {
                            //     /* permission check*/
                            //     // int st = permission_walk(cmount, file_name, g_UID, g_GID,
                            //     //                               PERM_WALK_CHECK_WRITE);
                            //     // if(st) fdc.read_only = 1;

                            // }
                            WCHAR absPath[MAX_PATH_CEPH];
                            GetAbsPath(absPath, MAX_PATH_CEPH, FileName);
                            DWORD attrFlag =  GetFileAttributesW(absPath);
                            if (attrFlag & FILE_ATTRIBUTE_READONLY) {
                                HANDLE  handle = CreateFileW(L"readonly.txt", AccessMode,//GENERIC_READ|GENERIC_WRITE|GENERIC_EXECUTE,
                                        ShareMode,
                                        NULL, // security attribute
                                        CreationDisposition,
                                        FlagsAndAttributes,// |FILE_FLAG_NO_BUFFERING,
                                        NULL); // template file handle

                                if (handle == INVALID_HANDLE_VALUE) {
                                    DWORD error = GetLastError();
                                    DdmPrintW(L"CreateFileW error code = %d\n", error);
                                    return error * -1; // error codes are negated value of Windows System Error codes
                                }
                                CloseHandle(handle);
                                
                                fdc.read_only = 1;
                            }
                        }
                        
                        if(fdc.read_only == 1)
                            fd = ceph_open(cmount, file_name, O_RDONLY, 0755);
                        else
                            fd = ceph_open(cmount, file_name, O_RDWR, 0755);
                        if(fd<0){
                            DbgPrint("\terror code = %d\n\n", fd);
                            fwprintf(stderr, L"CreateFile ceph_open REG OPEN_EXISTING error [%s][ret=%d]\n", FileName, fd);
                            return fd;
                        }
                        fdc.fd = fd;
                        memcpy(&(DokanFileInfo->Context), &fdc, sizeof(fdc));
                        /*fwprintf(stderr, L"CreateFile ceph_open REG OPEN_EXISTING OK [%s][fd=%d][Context=%d]\n", FileName, fd,
                            (int)DokanFileInfo->Context);*/
                        
                        return 0;
                    case CREATE_ALWAYS:
                        //open O_TRUNC & return ERROR_ALREADY_EXISTS
                        DdmPrintW(L"CreateFile %s CREATE_ALWAYS\n", FileName);
                        if(g_UseACL)
                        {
                            /* permission check*/
                            int st = permission_walk(cmount, file_name, g_UID, g_GID,
                                                          PERM_WALK_CHECK_READ|PERM_WALK_CHECK_WRITE);
                            if(st)
                                return -ERROR_ACCESS_DENIED;
                        }
                        fd = ceph_open(cmount, file_name, O_CREAT|O_TRUNC|O_RDWR, 0755);
                        if(fd<0){
                            DbgPrint("\terror code = %d\n\n", fd);
                            fwprintf(stderr, L"CreateFile ceph_open error REG CREATE_ALWAYS [%s][ret=%d]\n", FileName, fd);
                            return fd;
                        }
                        
                        fdc.fd = fd;
                        memcpy(&(DokanFileInfo->Context), &fdc, sizeof(fdc));
                        //fwprintf(stderr, L"CreateFile ceph_open REG CREATE_ALWAYS OK [%s][fd=%d][Context=%d]\n", FileName, fd,
                        //    (int)DokanFileInfo->Context);
                        
                        return ERROR_ALREADY_EXISTS;
                }
            }
            else if(S_ISDIR(st_buf.st_mode))
            {
                //DokanFileInfo->IsDirectory = TRUE;
                
                switch (CreationDisposition) {
                    case CREATE_NEW:
                        return -ERROR_FILE_EXISTS;
                    case TRUNCATE_EXISTING:
                        return 0;
                    case OPEN_ALWAYS:
                        return ERROR_ALREADY_EXISTS;
                    case OPEN_EXISTING:
                        return 0;
                    case CREATE_ALWAYS:
                        return ERROR_ALREADY_EXISTS;
                }
            }else
                return -1;
        }
        else /*File Not Exists*/
        {
            switch (CreationDisposition) {
                case CREATE_NEW:  //新建文件
                     DdmPrintW(L"CreateNew %s\n", FileName);
                    if(g_UseACL)
                    {
                        /* permission check*/
                        int st = permission_walk_parent(cmount, file_name, g_UID, g_GID,
                                                      PERM_WALK_CHECK_WRITE|PERM_WALK_CHECK_EXEC);
                        if(st)
                            return -ERROR_ACCESS_DENIED;
                    }

                    //查看权限
                    {
                        unsigned short auth = 0;
                        WCHAR absPath[MAX_PATH_CEPH] = { 0 };
                        GetAbsPath(absPath, MAX_PATH_CEPH, FileName);
                        char strGbk[MAX_PATH_CEPH] = { 0 };
                        wchar_to_char(strGbk, absPath, MAX_PATH_CEPH);
                        if (!FileCreate(strGbk, FALSE)) {
                            DdmPrintW(L"CreateFile %s ERROR_ACCESS_DENIED\n", FileName);
                            return -ERROR_ACCESS_DENIED;
                        }
                    }
                   
                    
                   
                    fd = ceph_open(cmount, file_name, O_CREAT|O_RDWR|O_EXCL, 0755);
                    if(fd<0){
                        DbgPrint("\terror code = %d\n\n", fd);
                        fwprintf(stderr, L"CreateFile NOF CREATE_NEW ceph_open error [%s][ret=%d]\n", FileName, fd);
                        return -1;
                    }
                    
                    fdc.fd = fd;
                    memcpy(&(DokanFileInfo->Context), &fdc, sizeof(fdc));
                    //fwprintf(stderr, L"CreateFile ceph_open NOF CREATE_NEW OK [%s][fd=%d][Context=%d]\n", FileName, fd,
                    //    (int)DokanFileInfo->Context);
                    
                    ceph_chown(cmount, file_name, g_UID, g_GID);
                    fuse_init_acl(cmount, file_name, 00777); //S_IRWXU|S_IRWXG|S_IRWXO
                    return 0;
                case CREATE_ALWAYS:
                    //create & return 0
                    if(g_UseACL)
                    {
                        /* permission check*/
                        int st = permission_walk_parent(cmount, file_name, g_UID, g_GID,
                                                      PERM_WALK_CHECK_WRITE|PERM_WALK_CHECK_EXEC);
                        if(st)
                            return -ERROR_ACCESS_DENIED;
                    }

                    DdmPrintW(L"CreateAlways %s\n", FileName);


                    //查看权限
                    {
                        unsigned short auth = 0;
                        WCHAR absPath[MAX_PATH_CEPH] = { 0 };
                        GetAbsPath(absPath, MAX_PATH_CEPH, FileName);
                        char strGbk[MAX_PATH_CEPH] = { 0 };
                        wchar_to_char(strGbk, absPath, MAX_PATH_CEPH);
                        if (!FileCreate(strGbk, FALSE)) {
                            DdmPrintW(L"CreateFile %s ERROR_ACCESS_DENIED\n", FileName);
                            return -ERROR_ACCESS_DENIED;
                        }
                    }

                    fd = ceph_open(cmount, file_name, O_CREAT|O_TRUNC|O_RDWR, 0755);
                    if(fd<0){
                        DbgPrint("\terror code = %d\n\n", fd);
                        fwprintf(stderr, L"CreateFile NOF CREATE_ALWAYS ceph_open error [%s][ret=%d]\n", FileName, fd);
                        return -1;
                    }
                    
                    fdc.fd = fd;
                    memcpy(&(DokanFileInfo->Context), &fdc, sizeof(fdc));
                    //fwprintf(stderr, L"CreateFile ceph_open NOF CREATE_ALWAYS_ALWAYS OK [%s][fd=%d][Context=%d]\n", FileName, fd,
                    //    (int)DokanFileInfo->Context);
                    
                    ceph_chown(cmount, file_name, g_UID, g_GID);
                    fuse_init_acl(cmount, file_name, 00777); //S_IRWXU|S_IRWXG|S_IRWXO
                    return 0;
                case OPEN_ALWAYS:
                    if(g_UseACL)
                    {
                        /* permission check*/
                        int st = permission_walk_parent(cmount, file_name, g_UID, g_GID,
                                                      PERM_WALK_CHECK_WRITE|PERM_WALK_CHECK_EXEC);
                        if(st)
                            return -ERROR_ACCESS_DENIED;
                    }
                    DdmPrintW(L"OpenAlways %s\n", FileName);

                    //查看权限
                    {
                        unsigned short auth = 0;
                        WCHAR absPath[MAX_PATH_CEPH] = { 0 };
                        GetAbsPath(absPath, MAX_PATH_CEPH, FileName);
                        char strGbk[MAX_PATH_CEPH] = { 0 };
                        wchar_to_char(strGbk, absPath, MAX_PATH_CEPH);
                        if (!FileCreate(strGbk, FALSE)) {
                            DdmPrintW(L"CreateFile %s ERROR_ACCESS_DENIED\n", FileName);
                            return -ERROR_ACCESS_DENIED;
                        }
                    }

                    fd = ceph_open(cmount, file_name, O_CREAT|O_RDWR, 0755);
                    if(fd<=0){
                        DbgPrint("\terror code = %d\n\n", fd);
                        fwprintf(stderr, L"CreateFile REG NOF OPEN_ALWAYS ceph_open error [%s][ret=%d]\n", FileName, fd);
                        return -1;
                    }
                    
                    fdc.fd = fd;
                    memcpy(&(DokanFileInfo->Context), &fdc, sizeof(fdc));
   
                    ceph_chown(cmount, file_name, g_UID, g_GID);
                    fuse_init_acl(cmount, file_name, 00777); //S_IRWXU|S_IRWXG|S_IRWXO
                    return 0;
                case OPEN_EXISTING:
                    if (file_name[0] == '/')
                        return -ERROR_FILE_NOT_FOUND;
                    else
                        return 0;
                case TRUNCATE_EXISTING:
                    return -ERROR_FILE_NOT_FOUND;
            }
        }
    }
    
    return -1;
}


static int
WinCephCreateDirectory(
    LPCWSTR                    FileName,
    PDOKAN_FILE_INFO        DokanFileInfo)
{
    WCHAR filePath[MAX_PATH_CEPH];
    GetFilePath(filePath, MAX_PATH_CEPH, FileName);

    DdmPrintW(L"CreateDirectory : %s\n", filePath);
    DokanResetTimeout(CEPH_DOKAN_IO_TIMEOUT, DokanFileInfo);
    
    char file_name[MAX_PATH_CEPH];
    int len = wchar_to_char(file_name, FileName, MAX_PATH_CEPH);
    ToLinuxFilePath(file_name);
    
    //fwprintf(stderr, L"CreateDirectory : %s\n", filePath);
    if(strcmp(file_name, "/")==0)
    {
        return 0;
    }
    
    // if(g_UseACL)
    // {
    //     /* permission check*/
    //     int st = permission_walk_parent(cmount, file_name, g_UID, g_GID,
    //                                   PERM_WALK_CHECK_WRITE|PERM_WALK_CHECK_EXEC);
    //     if(st)
    //         return -ERROR_ACCESS_DENIED;
    // }
    
    // struct stat st_buf;
    // int ret = ceph_stat(cmount, file_name, &st_buf);
    // if(ret==0){
    //     if(S_ISDIR(st_buf.st_mode)){
    //         fwprintf(stderr, L"CreateDirectory ceph_mkdir EXISTS [%s][ret=%d]\n", FileName, ret);
    //         return -ERROR_ALREADY_EXISTS;
    //     }
    // }
    
    

    // { //查询文件夹是否存在
    //     WCHAR absPath[MAX_PATH_CEPH] = { 0 };
    //     GetAbsPath(absPath, MAX_PATH_CEPH, FileName);
    //     ctx_common request;
    //     memset(&request, 0, sizeof(request));
    //     wchar_to_char(request.path, absPath, MAX_PATH_CEPH);
    //     HANDLE h = PipeConnect(CEPH_CHANNEL_NAME);

    //     if (SendDataToUI2(h, DDM_FILESTATUS, (void*)&request, sizeof(request)) !=  sizeof(request)) {
    //         DdmPrintW(L"Create Failed, PipeSend Error\n");
    //         return -ERROR_UNEXP_NET_ERR;
    //     }


    //     pDdm_msg_ret pResult = (pDdm_msg_ret)malloc(sizeof(ddm_msg_ret));
    //     int iRecv = ReadDataFromUI2(h, (void*)pResult, sizeof(ddm_msg_ret));
    //     PipeClose(h);
    //     int ret = pResult->result;
    //     free(pResult);
    //     //文件不存在
    //     if (ret == RET_ALLOW) {
    //         return -ERROR_ALREADY_EXISTS;
    //     } else if(ret == RET_FAILED) {
    //         struct stat st_buf;
    //         int ret = ceph_stat(cmount, file_name, &st_buf);
    //         if(ret==0){
    //             if(S_ISDIR(st_buf.st_mode)) {
    //                 return -ERROR_ALREADY_EXISTS;
    //             }
    //         }
    //     }
    // }


    //查看权限
    {
        unsigned short auth = 0;
        WCHAR absPath[MAX_PATH_CEPH] = { 0 };
        GetAbsPath(absPath, MAX_PATH_CEPH, FileName);
        char strGbk[MAX_PATH_CEPH] = { 0 };
        wchar_to_char(strGbk, absPath, MAX_PATH_CEPH);
       if (!FileCreate(strGbk, TRUE)) {
            DdmPrintW(L"CreateDirectory %s ERROR_ACCESS_DENIED\n", FileName);
            return -ERROR_ACCESS_DENIED;
        }
    }
    //文件是否存在
    struct stat st_buf;
    int ret = ceph_stat(cmount, file_name, &st_buf);
    if(ret==0){
        if(S_ISDIR(st_buf.st_mode)) {
            return -ERROR_ALREADY_EXISTS;
        }
    }
    
    ret = ceph_mkdir(cmount, file_name, 0755);
    if(ret == -2)
    {
        fwprintf(stderr, L"CreateDirectory ceph_mkdir ENOENT [%s][ret=%d]\n", FileName, ret);
        return -ERROR_PATH_NOT_FOUND;
    }else if(ret){
        DbgPrint("\terror code = %d\n\n", ret);
        fwprintf(stderr, L"CreateDirectory ceph_mkdir ERROR [%s][ret=%d]\n", FileName, ret);
        return -5;
    }
    
    if(g_UseACL){
	    ceph_chown(cmount, file_name, g_UID, g_GID);
	    fuse_init_acl(cmount, file_name, 0040777); //S_IRWXU|S_IRWXG|S_IRWXO|S_IFDIR
	}
    return 0;
}

static int
WinCephOpenDirectory(
    LPCWSTR                    FileName,
    PDOKAN_FILE_INFO        DokanFileInfo)
{
    WCHAR filePath[MAX_PATH_CEPH];
    DWORD attr;

    //GetFilePath(filePath, MAX_PATH_CEPH, FileName);
    wcscpy(filePath, FileName);

   // DdmPrintW(L"OpenDirectory : %s\n", filePath);
    DokanResetTimeout(CEPH_DOKAN_IO_TIMEOUT, DokanFileInfo);
    
    char file_name[MAX_PATH_CEPH];
    int len = wchar_to_char(file_name, filePath, MAX_PATH_CEPH);
    ToLinuxFilePath(file_name);
    
    //fwprintf(stderr, L"OpenDirectoryLinux : %s\n", FileName);
    
    struct stat st_buf;
    int ret = ceph_stat(cmount, file_name, &st_buf);
    if(ret){
        DbgPrint("\terror code = %d\n\n", ret);
        fwprintf(stderr, L"OpenDirectory ceph_stat ERROR [%s][ret=%d]\n", FileName, ret);
        return -1;
    }
    
    if(g_UseACL)
    {
        /* permission check*/
        int st = permission_walk(cmount, file_name, g_UID, g_GID,
                                      PERM_WALK_CHECK_READ|PERM_WALK_CHECK_EXEC);
        if(st)
            return -ERROR_ACCESS_DENIED;
    }
    
    if(S_ISDIR(st_buf.st_mode)){
        int fd = ceph_open(cmount, file_name, O_RDONLY, 0755);
        if(fd <= 0){
            DbgPrint("OpenDirectory ceph_opendir error : %s [%d]\n", filePath, ret);
            fwprintf(stderr, L"OpenDirectory ceph_opendir error : %s [fd:%d]\n", FileName, fd);
            return -1;
        }
        
        struct fd_context fdc;
        memset(&fdc, 0, sizeof(struct fd_context));
        
        fdc.fd = fd;
        memcpy(&(DokanFileInfo->Context), &fdc, sizeof(fdc));

        //DokanFileInfo->IsDirectory = TRUE;
        
        //fwprintf(stderr, L"OpenDirectory OK : %s [%d]\n", FileName, dirp);
        
        return 0;
    }
    else
        return -1;
}

static int
WinCephCloseFile(
    LPCWSTR                    FileName,
    PDOKAN_FILE_INFO        DokanFileInfo)
{
    WCHAR filePath[MAX_PATH_CEPH];
    GetFilePath(filePath, MAX_PATH_CEPH, FileName);

    DokanResetTimeout(CEPH_DOKAN_IO_TIMEOUT, DokanFileInfo);

    if (DokanFileInfo->Context) {
        DbgPrintW(L"CloseFile: %s\n", filePath);
        DbgPrintW(L"\terror : not cleanuped file\n\n");
        
        char file_name[MAX_PATH_CEPH];
        int len = wchar_to_char(file_name, FileName, MAX_PATH_CEPH);
        ToLinuxFilePath(file_name);
        
        struct fd_context fdc;
        memcpy(&fdc, &(DokanFileInfo->Context), sizeof(fdc));
        
        //fwprintf(stderr, L"ceph_close [%s][fd=%d]\n", FileName, fd);
        
        int ret = ceph_close(cmount, fdc.fd);
        if(ret){
            DbgPrint("\terror code = %d\n\n", ret);
            //return ret;
        }
        
        DokanFileInfo->Context = 0;
        
        if(fdc.delete_on_close)
        {
            if(DokanFileInfo->IsDirectory == FALSE)
            {
                int ret = ceph_unlink(cmount, file_name);
                if (ret != 0) {
                    DbgPrintW(L"DeleteOnClose ceph_unlink error code = %d\n\n", ret);
                } else {
                    DbgPrintW(L"DeleteOnClose ceph_unlink success\n\n");
                }
                fwprintf(stderr, L"fdc.delete_on_close [%s]\n", FileName);
            }
        }
    } else {
        DbgPrintW(L"Close: %s\n\tinvalid handle\n\n", filePath);
        return 0;
    }

    DbgPrintW(L"\n");
    return 0;
}


static int
WinCephCleanup(
    LPCWSTR                    FileName,
    PDOKAN_FILE_INFO        DokanFileInfo)
{
    WCHAR filePath[MAX_PATH_CEPH];
    GetFilePath(filePath, MAX_PATH_CEPH, FileName);

    DokanResetTimeout(CEPH_DOKAN_IO_TIMEOUT, DokanFileInfo);
    
    char file_name[MAX_PATH_CEPH];
    int len = wchar_to_char(file_name, FileName, MAX_PATH_CEPH);
    ToLinuxFilePath(file_name);

    if (DokanFileInfo->Context) {
        if (DokanFileInfo->DeleteOnClose) {
            DbgPrintW(L"\tDeleteOnClose\n");
            //fwprintf(stderr, L"Cleanup DeleteOnClose: %s\n", filePath);
            if (DokanFileInfo->IsDirectory) {
                DbgPrintW(L"  DeleteDirectory ");
                //fwprintf(stderr, L"cleanup ceph_rmdir [%s]\n", FileName);
                int ret = ceph_rmdir(cmount, file_name);
                if (ret != 0) {
                    DbgPrintW(L"error code = %d\n\n", ret);
                } else {
                    DbgPrintW(L"success\n\n");
                }
            } else {
                DbgPrintW(L"  DeleteFile ");
                //fwprintf(stderr, L"cleanup ceph_unlink [%s]\n", FileName);
                int ret = ceph_unlink(cmount, file_name);
                if (ret != 0) {
                    DbgPrintW(L" error code = %d\n\n", ret);
                } else {
                    DbgPrintW(L"success\n\n");
                }
            }
        }

    } else {
        DbgPrintW(L"Cleanup: %s\n\tinvalid handle\n\n", filePath);
        return -1;
    }

    return 0;
}


static int
WinCephReadFile(
    LPCWSTR              FileName,
    LPVOID               Buffer,
    DWORD                BufferLength,
    LPDWORD              ReadLength,
    LONGLONG             Offset,
    PDOKAN_FILE_INFO     DokanFileInfo)
{
    WCHAR    filePath[MAX_PATH_CEPH];
    BOOL    opened = FALSE;
    if(Offset > 1024*1024*1024*1024LL || Offset < 0 || BufferLength < 0
          || BufferLength > 128*1024*1024){
        fwprintf(stderr, L"FIlE WIRTE TOO LARGE [fn:%s][Offset=%lld][BufferLength=%ld]\n",FileName, Offset, BufferLength);
        return -1; //ERROR_FILE_TOO_LARGE
    }
    if(BufferLength == 0)
    {
        *ReadLength = 0;
        return 0;
    }
    
    GetFilePath(filePath, MAX_PATH_CEPH, FileName);

    DbgPrintW(L"ReadFile : %s\n", filePath);

    DokanResetTimeout(CEPH_DOKAN_IO_TIMEOUT, DokanFileInfo);

    char file_name[MAX_PATH_CEPH];
    int len = wchar_to_char(file_name, FileName, MAX_PATH_CEPH);
    ToLinuxFilePath(file_name);
    
    if(BufferLength == 0)
    {
        fwprintf(stderr, L"ceph_read BufferLength==0 [fn:%s][Offset=%ld]\n",FileName, Offset);
        *ReadLength = 0;
        return 0;
    }

    //fwprintf(stderr, L"ceph_read [Offset=%lld][BufferLength=%ld]\n", Offset, BufferLength);
    struct fd_context fdc;
    memcpy(&fdc, &(DokanFileInfo->Context), sizeof(fdc));
    if(fdc.fd == 0){
        char file_name[MAX_PATH_CEPH];
        int len = wchar_to_char(file_name, FileName, MAX_PATH_CEPH);
        ToLinuxFilePath(file_name);
        
        fwprintf(stderr, L"ceph_read reopen fd [fn:%s][Offset=%ld]\n", FileName, Offset);
        
        int fd_new = ceph_open(cmount, file_name, O_RDONLY, 0);
        if(fd_new < 0)
        {
            fwprintf(stderr, L"ceph_read reopen fd [fn:%s][fd_new=%d][Offset=%ld]\n", FileName, fd_new, Offset);
            return -1;
        }
        ////////////////解密
        // uint8_t* pDncode = (uint8_t*)malloc(BufferLength * sizeof(uint8_t));
        // memset(pDncode, 0, BufferLength * sizeof(uint8_t));
        // int ret = ceph_read(cmount, fd_new, (char*)pDncode, BufferLength, Offset);
        // SM4_ctr128_decrypt(pDncode, (uint8_t*)Buffer, ret, Offset);
        // free(pDncode);

        //////////////不解密
        int ret = ceph_read(cmount, fd_new, Buffer, BufferLength, Offset);

        if(ret<0)
        {
            fwprintf(stderr, L"ceph_read IO error [Offset=%ld][ret=%d]\n", Offset, ret);
            ceph_close(cmount, fd_new);
            return ret;
        }
        *ReadLength = ret;
        ceph_close(cmount, fd_new);
        return 0;
    }
    else{
        ////////////////解密
        // uint8_t* pDncode = (uint8_t*)malloc(BufferLength * sizeof(uint8_t));
        // memset(pDncode, 0, BufferLength * sizeof(uint8_t));
        // int ret = ceph_read(cmount, fdc.fd, (char*)pDncode, BufferLength, Offset);
        // SM4_ctr128_decrypt(pDncode, (uint8_t*)Buffer, ret, Offset);
        // free(pDncode);

        //////////////不解密
        int ret = ceph_read(cmount, fdc.fd, Buffer, BufferLength, Offset);
        if(ret<0)
        {
            fwprintf(stderr, L"ceph_read IO error [Offset=%ld][ret=%d]\n", Offset, ret);
            return ret;
        }
        *ReadLength = ret;
        
        return 0;
    }
}


static int
WinCephWriteFile(
    LPCWSTR        FileName,
    LPCVOID        Buffer,
    DWORD        NumberOfBytesToWrite,
    LPDWORD        NumberOfBytesWritten,
    LONGLONG            Offset,
    PDOKAN_FILE_INFO    DokanFileInfo)
{
    BOOL    opened = FALSE;
    WCHAR    filePath[MAX_PATH_CEPH];
    if(Offset > 1024*1024*1024*1024LL || Offset < 0 || NumberOfBytesToWrite < 0
          || NumberOfBytesToWrite > 128*1024*1024){
        fwprintf(stderr, L"FIlE WIRTE TOO LARGE [fn:%s][Offset=%lld][NumberOfBytesToWrite=%ld]\n", FileName, Offset, NumberOfBytesToWrite);
        return -1; //ERROR_FILE_TOO_LARGE
    }
    if(NumberOfBytesToWrite == 0)
    {
        *NumberOfBytesWritten = 0;
        return 0;
    }
    DokanResetTimeout(CEPH_DOKAN_IO_TIMEOUT, DokanFileInfo);

    DbgPrintW(L"WriteFile : %s, offset %I64d, length %d\n", filePath, Offset, NumberOfBytesToWrite);

    char file_name[MAX_PATH_CEPH];
    int len = wchar_to_char(file_name, FileName, MAX_PATH_CEPH);
    ToLinuxFilePath(file_name);

    //fwprintf(stderr, L"ceph_write [Offset=%lld][NumberOfBytesToWrite=%ld]\n", Offset, NumberOfBytesToWrite);
    struct fd_context fdc;
    memcpy(&fdc, &(DokanFileInfo->Context), sizeof(fdc));
    
    if(fdc.read_only == 1) {
        DdmPrintW(L"WriteFile %s: ERROR_ACCESS_DENIED\n", FileName);
        return -ERROR_ACCESS_DENIED;
    }
        
    
    if(fdc.fd==0){
        char file_name[MAX_PATH_CEPH];
        int len = wchar_to_char(file_name, FileName, MAX_PATH_CEPH);
        ToLinuxFilePath(file_name);
        
        fwprintf(stderr, L"ceph_write reopen fd [fn:%s][Offset=%ld]\n",FileName, Offset);
        
        int fd_new = ceph_open(cmount, file_name, O_RDONLY, 0);
        if(fd_new < 0)
        {
            fwprintf(stderr, L"ceph_write reopen fd [fn:%s][fd_new=%d][Offset=%ld]\n", FileName, fd_new, Offset);
            return -1;
        }
        ////////////////加密
        // uint8_t* pEncode = (uint8_t*)malloc(NumberOfBytesToWrite * sizeof(uint8_t));
        // memset(pEncode, 0, NumberOfBytesToWrite * sizeof(uint8_t));
        // SM4_ctr128_encrypt((uint8_t*)Buffer, pEncode, NumberOfBytesToWrite, Offset);
        // int ret = ceph_write(cmount, fd_new, (char*)pEncode, NumberOfBytesToWrite, Offset);
        // free(pEncode);

        ///////////////不加密
        int ret = ceph_write(cmount, fd_new, Buffer, NumberOfBytesToWrite, Offset);

        if(ret<0)
        {
            fwprintf(stderr, L"ceph_write IO error [fn:%s][fd=%d][Offset=%lld][Length=%ld]\n", FileName, fd_new, Offset, NumberOfBytesToWrite);
            ceph_close(cmount, fd_new);
            return ret;
        }
        *NumberOfBytesWritten = ret;
        
        ceph_close(cmount, fd_new);
        return 0;
    }
    else{
        ////////////////加密
        // uint8_t* pEncode = (uint8_t*)malloc(NumberOfBytesToWrite * sizeof(uint8_t));
        // memset(pEncode, 0, NumberOfBytesToWrite * sizeof(uint8_t));
        // SM4_ctr128_encrypt((uint8_t*)Buffer, pEncode, NumberOfBytesToWrite, Offset);
        // int ret = ceph_write(cmount, fdc.fd, (char*)pEncode, NumberOfBytesToWrite, Offset);
        // free(pEncode);


        ///////////////不加密
        int ret = ceph_write(cmount, fdc.fd, Buffer, NumberOfBytesToWrite, Offset);
        if(ret<0)
        {
            fwprintf(stderr, L"ceph_write IO error [fn:%s][fd=%d][Offset=%lld][Length=%ld]\n", FileName, fdc.fd, Offset, NumberOfBytesToWrite);
            return ret;
        }
        *NumberOfBytesWritten = ret;
        
        return 0;
    }
}


static int
WinCephFlushFileBuffers(
    LPCWSTR        FileName,
    PDOKAN_FILE_INFO    DokanFileInfo)
{
    WCHAR    filePath[MAX_PATH_CEPH];

    GetFilePath(filePath, MAX_PATH_CEPH, FileName);

    DbgPrintW(L"FlushFileBuffers : %s\n", filePath);
    fwprintf(stderr, L"FlushFileBuffers : %s\n", filePath);

    DokanResetTimeout(CEPH_DOKAN_IO_TIMEOUT, DokanFileInfo);

    char file_name[MAX_PATH_CEPH];
    int len = wchar_to_char(file_name, FileName, MAX_PATH_CEPH);
    ToLinuxFilePath(file_name);
    
    struct fd_context fdc;
    memcpy(&fdc, &(DokanFileInfo->Context), sizeof(fdc));
    if(fdc.fd==0){
        fwprintf(stderr, L"ceph_sync FD error [%s] fdc is NULL\n", FileName);
        return -1;
    }
    
    int ret = ceph_fsync(cmount, fdc.fd, 0);
    if(ret){
        fwprintf(stderr, L"ceph_sync error [%s][%df]\n", FileName, fdc.fd);
        return -1;
    }

    return 0;
}

static int
WinCephGetFileInformation(
    LPCWSTR                            FileName,
    LPBY_HANDLE_FILE_INFORMATION    HandleFileInformation,
    PDOKAN_FILE_INFO                DokanFileInfo)
{
    WCHAR    filePath[MAX_PATH_CEPH];

    GetFilePath(filePath, MAX_PATH_CEPH, FileName);

    DbgPrintW(L"GetFileInfo : %s\n", filePath);   

    DokanResetTimeout(CEPH_DOKAN_IO_TIMEOUT, DokanFileInfo);

    memset(HandleFileInformation, 0, sizeof(BY_HANDLE_FILE_INFORMATION));
    
    char file_name[MAX_PATH_CEPH];
    int len = wchar_to_char(file_name, FileName, MAX_PATH_CEPH);
    ToLinuxFilePath(file_name);
    
    struct stat stbuf;
    struct fd_context fdc;
    memcpy(&fdc, &(DokanFileInfo->Context), sizeof(fdc));
    if (fdc.fd==0) {
        int ret = ceph_stat(cmount, file_name, &stbuf);
        if(ret){
            //fwprintf(stderr, L"GetFileInformation ceph_stat error [%s]\n", FileName);
            return -1;
        }
    }else{
        int ret = ceph_fstat(cmount, fdc.fd, &stbuf);
        if(ret){
            fwprintf(stderr, "GetFileInformation ceph_fstat error [%s]\n", FileName);
            return -1;
        }
    }
    
    //fwprintf(stderr, L"GetFileInformation1 [%s][size:%lld][time:%lld]\n", FileName, stbuf.st_size, stbuf.st_mtim.tv_sec);
    //fill stbuf.st_size
    HandleFileInformation->nFileSizeLow = (stbuf.st_size << 32)>>32;
    HandleFileInformation->nFileSizeHigh = stbuf.st_size >> 32;
    
    //fill stbuf.st_mtim
    UnixTimeToFileTime(stbuf.st_mtime, &HandleFileInformation->ftCreationTime);
    UnixTimeToFileTime(stbuf.st_mtime, &HandleFileInformation->ftLastAccessTime);
    UnixTimeToFileTime(stbuf.st_mtime, &HandleFileInformation->ftLastWriteTime);
    
    //fwprintf(stderr, L"GetFileInformation6 [%s][size:%lld][time a:%lld m:%lld c:%lld]\n", 
    //    FileName, stbuf.st_size, stbuf.st_atim.tv_sec, stbuf.st_mtim.tv_sec, stbuf.st_ctim.tv_sec);
    
    //fill stbuf.st_mode
    if(S_ISDIR(stbuf.st_mode)){
        HandleFileInformation->dwFileAttributes |= FILE_ATTRIBUTE_DIRECTORY;
    }
    else if(S_ISREG(stbuf.st_mode)){
        WCHAR absPath[MAX_PATH_CEPH] = { 0 };
        GetAbsPath(absPath, MAX_PATH_CEPH, FileName);
        char utf8path[MAX_PATH_CEPH] = { 0 };
        wchar_to_char(utf8path, absPath, MAX_PATH_CEPH);
        unsigned short nAuth =  FileAuth(utf8path);
        //DdmPrintW(L"CephGetFileInformation %s  nAuth:%d\n", FileName, nAuth);
        if ((nAuth & AUTH_READ) && (!(nAuth & AUTH_WRITE))) {
            HandleFileInformation->dwFileAttributes = (FILE_ATTRIBUTE_READONLY | FILE_ATTRIBUTE_ARCHIVE);
            //DdmPrintW(L"CephGetFileInformation ReadOnly %s\n", FileName);
        } else {
            HandleFileInformation->dwFileAttributes = FILE_ATTRIBUTE_ARCHIVE;
            //DdmPrintW(L"CephGetFileInformation NORMAL %s\n", FileName);
        }
        
    }
    
    //fill stbuf.st_ino
    HandleFileInformation->nFileIndexLow = (stbuf.st_ino << 32)>>32;
    HandleFileInformation->nFileIndexHigh = stbuf.st_ino >> 32;
    
    //fill stbuf.st_nlink
    HandleFileInformation->nNumberOfLinks = stbuf.st_nlink;
    
    return 0;
}

static int
WinCephFindFiles(
    LPCWSTR                FileName,
    PFillFindData        FillFindData, // function pointer
    PDOKAN_FILE_INFO    DokanFileInfo)
{
    WCHAR                absFilePath[MAX_PATH_CEPH];
    WIN32_FIND_DATAW    findData;
    int count = 0;

    GetAbsPath(absFilePath, MAX_PATH_CEPH, FileName);
    DokanResetTimeout(CEPH_DOKAN_IO_TIMEOUT, DokanFileInfo);

    ctx_common ctx;
	ZeroMemory(&ctx, sizeof(ctx));
    wchar_to_char(ctx.path, absFilePath, MAX_PATH);
	ctx.isDirectory =  1;//DokanFileInfo->IsDirectory;
    
    HANDLE h = PipeConnect(CEPH_CHANNEL_NAME);

    if (SendDataToUI2(h, DDM_READDIR , (void*)&ctx, sizeof(ctx)) != sizeof(ctx)) {
        DdmPrintW(L"FindFiles SendDataToUI2 Error\n");
        return -1;
    }

    pDdm_msg_ret pResult = (pDdm_msg_ret)malloc(sizeof(ddm_msg_ret));
    ReadDataFromUI2(h, (void*)pResult, sizeof(ddm_msg_ret));
   
    //个人区, node 不缓存 直接取CEPH的列表
    if (pResult->result == RET_NOTFOUND) {
        char file_name[MAX_PATH_CEPH];
        wchar_to_char(file_name, FileName, MAX_PATH_CEPH);
        ToLinuxFilePath(file_name);
        struct ceph_dir_result *dirp;
        int ret = ceph_opendir(cmount, file_name, &dirp);
        if(ret != 0){
            DdmPrintW(L"ceph_opendir error : %s [%d]\n", FileName, ret);
            free(pResult);
            CloseHandle(h);
            return -1;
        }
    

        while(1)
        {
            memset(&findData, 0, sizeof(findData));
            struct dirent result;
            struct stat stbuf;
            int stmask;
            
            ret = ceph_readdirplus_r(cmount, dirp, &result, &stbuf, &stmask);
            //DdmPrintW(L"ceph_readdirplus_r ret : [%d]\n", ret);
            if(ret==0)
                break;
            if(ret<0){
                fprintf(stderr, "FindFiles ceph_readdirplus_r error [%ls][ret=%d]\n", FileName, ret);
                break;
            }
            
            if(strcmp(result.d_name, ".")==0 || strcmp(result.d_name, "..")==0){
            //     continue;
            }
            
            //d_name
            WCHAR d_name[MAX_PATH_CEPH];
            int len = char_to_wchar(d_name, result.d_name, MAX_PATH_CEPH);
            
            wcscpy(findData.cFileName, d_name);
            //st_size
            findData.nFileSizeLow = (stbuf.st_size << 32)>>32;
            findData.nFileSizeHigh = stbuf.st_size >> 32;
            
            //st_mtim
            UnixTimeToFileTime(stbuf.st_mtime, &findData.ftCreationTime);
            UnixTimeToFileTime(stbuf.st_mtime, &findData.ftLastAccessTime);
            UnixTimeToFileTime(stbuf.st_mtime, &findData.ftLastWriteTime);
            
            //st_mode
            if(S_ISDIR(stbuf.st_mode)){
                //printf("[%s] is a Directory.............\n", result.d_name);
                findData.dwFileAttributes |= FILE_ATTRIBUTE_DIRECTORY;
            }
            else if(S_ISREG(stbuf.st_mode)){
                //printf("[%s] is a Regular File.............\n", result.d_name);
                findData.dwFileAttributes |= FILE_ATTRIBUTE_NORMAL;
            }
            
            FillFindData(&findData, DokanFileInfo);
            count++;
        }
    
        ret = ceph_closedir(cmount, dirp);
    } else if (pResult->result == RET_ALLOW) {
        //共享区  FIXME：放着不动就崩了
        count = pResult->ctxLength / sizeof(ddm_fileInfo);
        //DdmPrintW(L"pResult->ctxLength = %d, count = %d\n", pResult->ctxLength, count);
        if (count > 0) {

            pDdm_fileInfo pArray = (pDdm_fileInfo)malloc(sizeof(ddm_fileInfo) * count);
            ReadDataFromUI2(h, (void*)pArray, sizeof(ddm_fileInfo) * count);

            for (unsigned int i = 0; i < count; i++) {
                ZeroMemory(&findData, sizeof(findData));
                char path[MAX_PATH_CEPH] = { 0 };
                wchar_to_char(path, FileName, MAX_PATH_CEPH);
                ToLinuxFilePath(path);
                strcat(path, "/");
                strcat(path, pArray[i].name);
                //文件名
                char_to_wchar(findData.cFileName, pArray[i].name, MAX_PATH_CEPH);
                struct stat stbuf;
                if (ceph_lstat(cmount, path, &stbuf)) {
                    DdmPrintW(L"file not exist: %s\n", findData.cFileName);
                    continue;
                }

                
                //st_size
                findData.nFileSizeLow = (stbuf.st_size << 32)>>32;
                findData.nFileSizeHigh = stbuf.st_size >> 32;
                
                //st_mtim
                UnixTimeToFileTime(stbuf.st_mtime, &findData.ftCreationTime);
                UnixTimeToFileTime(stbuf.st_mtime, &findData.ftLastAccessTime);
                UnixTimeToFileTime(stbuf.st_mtime, &findData.ftLastWriteTime);
                
                //st_mode
                if(S_ISDIR(stbuf.st_mode)){
                    findData.dwFileAttributes |= FILE_ATTRIBUTE_DIRECTORY;
                }
                else if(S_ISREG(stbuf.st_mode)) {
                    
                    if ((pArray[i].mode & AUTH_READ) && !(pArray[i].mode & AUTH_WRITE)) {
                        findData.dwFileAttributes |= FILE_ATTRIBUTE_READONLY;
                    } else {
                        findData.dwFileAttributes |= FILE_ATTRIBUTE_NORMAL;
                    }
                }
                FillFindData(&findData, DokanFileInfo);
            }
            free(pArray);
        }
    } else {
        DdmPrintW(L"FindFiles AccessDenfine: %s\n", FileName);
    }
    free(pResult);
    CloseHandle(h);
    return 0;
}


static int
WinCephDeleteFile(
    LPCWSTR                FileName,
    PDOKAN_FILE_INFO    DokanFileInfo)
{
    WCHAR    filePath[MAX_PATH_CEPH];

    GetFilePath(filePath, MAX_PATH_CEPH, FileName);
    DokanResetTimeout(CEPH_DOKAN_IO_TIMEOUT, DokanFileInfo);

    // char file_name[MAX_PATH_CEPH];
    // int len = wchar_to_char(file_name, FileName, MAX_PATH_CEPH);
    // ToLinuxFilePath(file_name);
    //不检查DAC
    // if(g_UseACL)
    // {
    //     /* permission check*/
    //     int st = permission_walk_parent(cmount, file_name, g_UID, g_GID,
    //                                   PERM_WALK_CHECK_WRITE|PERM_WALK_CHECK_EXEC);
    //     if(st)
    //         return -ERROR_ACCESS_DENIED;
    // }


    //cleanup和 close中会调用ceph接口删除文件
    //这里做权限判断
   
    
    WCHAR absPath[MAX_PATH_CEPH] = { 0 };
    GetAbsPath(absPath, MAX_PATH_CEPH, FileName);
    char strGbk[MAX_PATH_CEPH] = { 0 };
    wchar_to_char(strGbk, absPath, MAX_PATH_CEPH);
    if (!FileDelete(strGbk, FALSE)) {
        return -ERROR_ACCESS_DENIED;
    }
    DdmPrintW(L"DeleteFile %s\n", filePath);
    return 0;
}

static int
WinCephDeleteDirectory(
    LPCWSTR                FileName,
    PDOKAN_FILE_INFO    DokanFileInfo)
{
    WCHAR    filePath[MAX_PATH_CEPH];
    HANDLE    hFind;
    WIN32_FIND_DATAW    findData;
    ULONG    fileLen;

    ZeroMemory(filePath, sizeof(filePath));
    GetFilePath(filePath, MAX_PATH_CEPH, FileName);
    DokanResetTimeout(CEPH_DOKAN_IO_TIMEOUT, DokanFileInfo);

    WCHAR absPath[MAX_PATH_CEPH] = { 0 };
    GetAbsPath(absPath, MAX_PATH_CEPH, FileName);
    char strGbk[MAX_PATH_CEPH] = { 0 };
    wchar_to_char(strGbk, absPath, MAX_PATH_CEPH);
    if (!FileDelete(strGbk, TRUE)) {
        return -ERROR_ACCESS_DENIED;
    }
    DdmPrintW(L"DeleteDirectory %s\n", absPath);
    char file_name[MAX_PATH_CEPH];
    int len = wchar_to_char(file_name, FileName, MAX_PATH_CEPH);
    ToLinuxFilePath(file_name);
    

    // if(g_UseACL)
    // {
    //     /* permission check*/
    //     int st = permission_walk_parent(cmount, file_name, g_UID, g_GID,
    //                                   PERM_WALK_CHECK_WRITE|PERM_WALK_CHECK_EXEC);
    //     if(st)
    //         return -ERROR_ACCESS_DENIED;
    // }

    struct ceph_dir_result *dirp;
    int ret = ceph_opendir(cmount, file_name, &dirp);
    if(ret != 0){
        fwprintf(stderr, L"ceph_opendir error : %s [%d]\n", FileName, ret);
        return -1;
    }
    
    //fwprintf(stderr, L"DeleteDirectory ceph_opendir OK: %s\n", FileName);
    
    while(1)
    {
        memset(&findData, 0, sizeof(findData));
        struct dirent *result = ceph_readdir(cmount, dirp);
        if(result!=NULL)
        {
            if(strcmp(result->d_name, ".")!=0 
                && strcmp(result->d_name, "..")!=0)
            {
                ceph_closedir(cmount, dirp);
                DbgPrintW(L"  Directory is not empty: %s\n", findData.cFileName);
                return -(int)ERROR_DIR_NOT_EMPTY;
            }
        }else break;
    }
    
    ceph_closedir(cmount, dirp);
    return 0;
}


static int
WinCephMoveFile(
    LPCWSTR                FileName, // existing file name
    LPCWSTR                NewFileName,
    BOOL                ReplaceIfExisting,
    PDOKAN_FILE_INFO   DokanFileInfo)
{
    WCHAR            filePath[MAX_PATH_CEPH];
    WCHAR            newFilePath[MAX_PATH_CEPH];
    BOOL            status;

    GetFilePath(filePath, MAX_PATH_CEPH, FileName);
    GetFilePath(newFilePath, MAX_PATH_CEPH, NewFileName);

    DdmPrintW(L"MoveFile %s -> %s\n", filePath, newFilePath);

    DokanResetTimeout(CEPH_DOKAN_IO_TIMEOUT, DokanFileInfo);

    char file_name[MAX_PATH_CEPH];
    int len = wchar_to_char(file_name, FileName, MAX_PATH_CEPH);
    ToLinuxFilePath(file_name);

    char newfile_name[MAX_PATH_CEPH];
    len = wchar_to_char(newfile_name, NewFileName, MAX_PATH_CEPH);
    ToLinuxFilePath(newfile_name);
    
    // if(g_UseACL)
    // {
    //     /* permission check*/
    //     int st = permission_walk_parent(cmount, file_name, g_UID, g_GID,
    //                                   PERM_WALK_CHECK_WRITE|PERM_WALK_CHECK_EXEC);
    //     if(st)
    //         return -ERROR_ACCESS_DENIED;
    // }
 
    char str_pathfrom[MAX_PATH_CEPH] = {0};
    char str_pathto[MAX_PATH_CEPH] = {0};
    WCHAR absPath[MAX_PATH_CEPH];
    GetAbsPath(absPath, MAX_PATH_CEPH, FileName);
    wchar_to_char(str_pathfrom, absPath, sizeof(str_pathfrom));

    GetAbsPath(absPath, MAX_PATH_CEPH, NewFileName);
    wchar_to_char(str_pathto, absPath, sizeof(str_pathto));


    if (!ReplaceIfExisting) {
        // int ret = UI2TellMeFileExistWhere(str_pathto);
        // if (ret == 0) {
        //     DdmPrintW(L"file exist ui2.%s\n", absPath);
        //     return -ERROR_ALREADY_EXISTS;
        // }
        
        // if (ret == 2) {
        //     struct stat st_buf;
        //     if (0 == ceph_stat(cmount, newfile_name, &st_buf)) {
        //         DdmPrintW(L"file exist ceph.%s\n", FileName);
        //         return -ERROR_ALREADY_EXISTS;
        //     }
        // }

        struct stat st_buf;
        if (0 == ceph_stat(cmount, newfile_name, &st_buf)) {
            if (DokanFileInfo->IsDirectory &&  S_ISDIR(st_buf.st_mode) || 
                (!DokanFileInfo->IsDirectory &&  S_ISREG(st_buf.st_mode)) ) {
                DdmPrintW(L"file exist ceph.%s\n", FileName);
                return -ERROR_ALREADY_EXISTS;
            }
        }
    }

    if (!FileMove(str_pathfrom, str_pathto, DokanFileInfo->IsDirectory)) {
        return -ERROR_ACCESS_DENIED;
    }


    int ret = ceph_rename(cmount, file_name, newfile_name);
    if(ret){
        DdmPrintW(L"ceph_rename error code = %d\n\n", ret);
        return ret;
    }
    return ret;
}


static int
WinCephLockFile(
    LPCWSTR                FileName,
    LONGLONG            ByteOffset,
    LONGLONG            Length,
    PDOKAN_FILE_INFO    DokanFileInfo)
{
    WCHAR    filePath[MAX_PATH_CEPH];
//    HANDLE    handle;
    LARGE_INTEGER offset;
    LARGE_INTEGER length;

    GetFilePath(filePath, MAX_PATH_CEPH, FileName);

    DbgPrintW(L"LockFile %s\n", filePath);
    fwprintf(stderr, L"LockFile %s [offset:%lld][len:%lld]\n", filePath,ByteOffset,Length);

    return 0;
}


static int
WinCephSetEndOfFile(
    LPCWSTR                FileName,
    LONGLONG            ByteOffset,
    PDOKAN_FILE_INFO    DokanFileInfo)
{
    WCHAR            filePath[MAX_PATH_CEPH];
    LARGE_INTEGER    offset;

    GetFilePath(filePath, MAX_PATH_CEPH, FileName);

    DbgPrintW(L"SetEndOfFile %s, %I64d\n", filePath, ByteOffset);
    //fwprintf(stderr, L"SetEndOfFile %s, %I64d\n", filePath, ByteOffset);

    DokanResetTimeout(CEPH_DOKAN_IO_TIMEOUT, DokanFileInfo);
    
    char file_name[MAX_PATH_CEPH];
    int len = wchar_to_char(file_name, FileName, MAX_PATH_CEPH);
    ToLinuxFilePath(file_name);
    
    struct fd_context fdc;
    memcpy(&fdc, &(DokanFileInfo->Context), sizeof(fdc));
    if (fdc.fd==0) {
        DbgPrintW(L"\tinvalid handle\n\n");
        fwprintf(stderr, L"SetEndOfFile fdc is NULL [%s]\n", FileName);
        return -1;
    }
    
    //fwprintf(stderr, L"SetEndOfFile [%s][%d][ByteOffset:%lld]\n", FileName, fdc.fd, ByteOffset);
    
    int ret = ceph_ftruncate(cmount, fdc.fd, ByteOffset);
    if(ret){
        fwprintf(stderr, L"SetEndOfFile ceph_ftruncate error [%s][%d][ByteOffset:%lld]\n", FileName, ret, ByteOffset);
        return -1;
    }
    
    return 0;
}

static int
WinCephSetAllocationSize(
    LPCWSTR                FileName,
    LONGLONG            AllocSize,
    PDOKAN_FILE_INFO    DokanFileInfo)
{
    WCHAR            filePath[MAX_PATH_CEPH];
    LARGE_INTEGER    fileSize;

    GetFilePath(filePath, MAX_PATH_CEPH, FileName);

    DokanResetTimeout(CEPH_DOKAN_IO_TIMEOUT, DokanFileInfo);

    DbgPrintW(L"SetAllocationSize %s, %I64d\n", filePath, AllocSize);
    //fwprintf(stderr,L"SetAllocationSize %s, %I64d\n", filePath, AllocSize);

    char file_name[MAX_PATH_CEPH];
    int len = wchar_to_char(file_name, FileName, MAX_PATH_CEPH);
    ToLinuxFilePath(file_name);
    
    struct fd_context fdc;
    memcpy(&fdc, &(DokanFileInfo->Context), sizeof(fdc));
    if (fdc.fd==0) {
        DbgPrintW(L"\tinvalid handle\n\n");
        fwprintf(stderr, L"SetAllocationSize fdc is NULL [%s]\n", FileName);
        return -1;
    }
    
    fwprintf(stderr, L"SetAllocationSize [%s][%d][AllocSize:%lld]\n", FileName, fdc.fd, AllocSize);
    
    struct stat stbuf;
    int ret = ceph_fstat(cmount, fdc.fd, &stbuf);
    if(ret){
        fwprintf(stderr, L"SetAllocationSize ceph_stat error [%s][%d][AllocSize:%lld]\n", FileName, ret, AllocSize);
        return -1;
    }
    
    if(AllocSize < stbuf.st_size){
        int ret = ceph_ftruncate(cmount, fdc.fd, AllocSize);
        if(ret){
            fwprintf(stderr, L"SetAllocationSize ceph_ftruncate error [%s][%d][AllocSize:%lld]\n", FileName, ret, AllocSize);
            return -1;
        }
        
        return 0;
    }
    else{
        //fwprintf(stderr, L"SetAllocationSize ceph_ftruncate EQUAL no need [%s][%d][AllocSize:%lld]\n", FileName, ret, AllocSize);
    }
    
    return 0;
}


static int
WinCephSetFileAttributes(
    LPCWSTR             FileName,
    DWORD               FileAttributes,
    PDOKAN_FILE_INFO    DokanFileInfo)
{
    WCHAR    filePath[MAX_PATH_CEPH];
    
    GetFilePath(filePath, MAX_PATH_CEPH, FileName);

    DbgPrintW(L"SetFileAttributes %s\n", filePath);

    DokanResetTimeout(CEPH_DOKAN_IO_TIMEOUT, DokanFileInfo);

    char file_name[MAX_PATH_CEPH];
    int len = wchar_to_char(file_name, FileName, MAX_PATH_CEPH);
    ToLinuxFilePath(file_name);
    
    //fwprintf(stderr, L"SetFileAttributes [%s][%d]\n", FileName, FileAttributes);
    
    return 0;
}


static int
WinCephSetFileTime(
    LPCWSTR                FileName,
    CONST FILETIME*        CreationTime,
    CONST FILETIME*        LastAccessTime,
    CONST FILETIME*        LastWriteTime,
    PDOKAN_FILE_INFO       DokanFileInfo)
{
    WCHAR    filePath[MAX_PATH_CEPH];

    GetFilePath(filePath, MAX_PATH_CEPH, FileName);

    DbgPrintW(L"SetFileTime %s\n", filePath);

    /*
      SetFileTime Call has some bug on Microsoft Office programs.
      So Just Comment these code . Need to FIX!!!
    */

    //DokanResetTimeout(CEPH_DOKAN_IO_TIMEOUT, DokanFileInfo);
    
    //char file_name[MAX_PATH_CEPH];
    //int len = wchar_to_char(file_name, FileName, MAX_PATH_CEPH);
    //ToLinuxFilePath(file_name);
    
    //struct stat stbuf;
    //memset(&stbuf, 0, sizeof(stbuf));
    //
    //int mask = 0;
    //if(CreationTime != NULL)
    //{
    //    mask |= CEPH_SETATTR_MTIME;
    //    FileTimeToUnixTime(*LastWriteTime, &stbuf.st_mtim.tv_sec);
    //}
    //if(LastAccessTime != NULL)
    //{
    //    mask |= CEPH_SETATTR_ATIME;
    //    FileTimeToUnixTime(*LastAccessTime, &stbuf.st_atim.tv_sec);
    //}
    //if(LastWriteTime != NULL)
    //{
    //    mask |= CEPH_SETATTR_MTIME;
    //    FileTimeToUnixTime(*LastWriteTime, &stbuf.st_mtim.tv_sec);
    //}
    
    //fwprintf(stderr, L"SetFileTime [%s][st_atim:%lld][st_mtim:%lld]\n", FileName, stbuf.st_atim.tv_sec, stbuf.st_mtim.tv_sec);
    //
    //int ret = ceph_setattr(cmount, file_name, &stbuf, mask);
    //if(ret){
    //    fwprintf(stderr, L"SetFileTime ceph_setattr error [%s]\n", FileName);
    //    return -1;
    //}
    DbgPrintW(L"\n");
    return 0;
}

static int
WinCephUnlockFile(
    LPCWSTR                FileName,
    LONGLONG            ByteOffset,
    LONGLONG            Length,
    PDOKAN_FILE_INFO    DokanFileInfo)
{
    WCHAR    filePath[MAX_PATH_CEPH];
    LARGE_INTEGER    length;
    LARGE_INTEGER    offset;

    GetFilePath(filePath, MAX_PATH_CEPH, FileName);

    DbgPrintW(L"UnlockFile %s\n", filePath);
    fwprintf(stderr, L"UnlockFile %s [offset:%lld][len:%lld]\n", filePath,ByteOffset,Length);

    return 0;
}

static int
WinCephGetFakeFileSecurity(
    LPCWSTR                    FileName,
    PSECURITY_INFORMATION    SecurityInformation,
    PSECURITY_DESCRIPTOR    SecurityDescriptor,
    ULONG                BufferLength,
    PULONG                LengthNeeded,
    PDOKAN_FILE_INFO    DokanFileInfo)
{
    HANDLE    handle;
    WCHAR     filePath[MAX_PATH_CEPH];
    BOOL      opened = FALSE;

    GetFilePath(filePath, MAX_PATH_CEPH, FileName);

    DbgPrintW(L"GetFileSecurity %s\n", filePath);
    
    char file_name[MAX_PATH_CEPH];
    int len = wchar_to_char(file_name, FileName, MAX_PATH_CEPH);
    ToLinuxFilePath(file_name);
    
    struct stat stbuf;
    int ret = ceph_stat(cmount, file_name, &stbuf);
    if(ret){
        fwprintf(stderr, L"GetFileSecurity ceph_stat error [%s]\n", FileName);
        return 0;
    }
    
    if(S_ISREG(stbuf.st_mode))
    {
        handle = CreateFile(
            Wceph_conf_file,
            GENERIC_READ|GENERIC_WRITE|GENERIC_EXECUTE,
            FILE_SHARE_READ|FILE_SHARE_WRITE,
            NULL,
            OPEN_EXISTING,
            0,
            NULL);
        if (handle == INVALID_HANDLE_VALUE) {
            DbgPrint(L"\tCreateFile error : %d\n\n", GetLastError());
            return -1;
        }
        opened = TRUE;
    }
    else if(S_ISDIR(stbuf.st_mode))
    {
        handle = CreateFile(
            L".",
            GENERIC_READ|GENERIC_EXECUTE,
            FILE_SHARE_READ,
            NULL,
            OPEN_EXISTING,
            FILE_FLAG_BACKUP_SEMANTICS,
            NULL);
        if (handle == INVALID_HANDLE_VALUE) {
            DbgPrint(L"\tCreateFile error : %d\n\n", GetLastError());
            return -1;
        }
        opened = TRUE;
    }
    else
        return 0;

    if(*SecurityInformation & SACL_SECURITY_INFORMATION != 0)
        *SecurityInformation &= (~SACL_SECURITY_INFORMATION);
    if (!GetUserObjectSecurity(handle, SecurityInformation, SecurityDescriptor,
            BufferLength, LengthNeeded)) {
        int error = GetLastError();
        if (error == ERROR_INSUFFICIENT_BUFFER) {
            DbgPrintW(L"  GetUserObjectSecurity failed: ERROR_INSUFFICIENT_BUFFER\n");
            if (opened)
                CloseHandle(handle);
            return error * -1;
        } else {
            DbgPrintW(L"  GetUserObjectSecurity failed: [err=%d][%ld]\n", error, *SecurityInformation);
            fwprintf(stderr, L"  GetUserObjectSecurity failed: [err=%d][%ld]\n", error, *SecurityInformation);
            if (opened)
                CloseHandle(handle);
            return error * -1;
        }
    }
    
    if (opened)
        CloseHandle(handle);
    
    return 0;
}

static int
WinCephGetFileSecurity(
    LPCWSTR                    FileName,
    PSECURITY_INFORMATION    SecurityInformation,
    PSECURITY_DESCRIPTOR    SecurityDescriptor,
    ULONG                BufferLength,
    PULONG                LengthNeeded,
    PDOKAN_FILE_INFO    DokanFileInfo)
{
    WCHAR    filePath[MAX_PATH_CEPH];
    GetFilePath(filePath, MAX_PATH_CEPH, FileName);

    DbgPrintW(L"GetFileSecurity %s\n", filePath);

    return WinCephGetFakeFileSecurity(FileName, SecurityInformation,
            SecurityDescriptor, BufferLength, LengthNeeded, DokanFileInfo);
}

static int
WinCephSetFileSecurity(
    LPCWSTR                    FileName,
    PSECURITY_INFORMATION    SecurityInformation,
    PSECURITY_DESCRIPTOR    SecurityDescriptor,
    ULONG                SecurityDescriptorLength,
    PDOKAN_FILE_INFO    DokanFileInfo)
{
    WCHAR filePath[MAX_PATH_CEPH];

    GetFilePath(filePath, MAX_PATH_CEPH, FileName);
    DbgPrintW(L"SetFileSecurity %s\n", filePath);

    return 0;
}

static int
WinCephGetVolumeInformation(
    LPWSTR        VolumeNameBuffer,
    DWORD        VolumeNameSize,
    LPDWORD        VolumeSerialNumber,
    LPDWORD        MaximumComponentLength,
    LPDWORD        FileSystemFlags,
    LPWSTR        FileSystemNameBuffer,
    DWORD        FileSystemNameSize,
    PDOKAN_FILE_INFO    DokanFileInfo)
{

    
    *VolumeSerialNumber = 0x19831116;
    *MaximumComponentLength = 256;
    *FileSystemFlags = FILE_CASE_SENSITIVE_SEARCH | 
                        FILE_CASE_PRESERVED_NAMES | 
                        FILE_SUPPORTS_REMOTE_STORAGE |
                        FILE_UNICODE_ON_DISK |
                        FILE_PERSISTENT_ACLS;

    // WCHAR* strUnicode = new wchar_t[strlen(LetterName) * sizeof(WCHAR)];  
    // wmemset(strUnicode, 0, strlen(LetterName) * sizeof(WCHAR));  
    MultiByteToWideChar(CP_ACP, 0, LetterName, -1, VolumeNameBuffer, (strlen(LetterName) + 1) * sizeof(WCHAR));  


    //wcscpy(VolumeNameBuffer, LetterName);
    wcscpy(FileSystemNameBuffer, VolumeNameBuffer);

    return 0;
}

static int
WinCephGetDiskFreeSpace(
    PULONGLONG FreeBytesAvailable, 
    PULONGLONG TotalNumberOfBytes,
    PULONGLONG TotalNumberOfFreeBytes,
    PDOKAN_FILE_INFO    DokanFileInfo)
{

    struct  stat st;
    int ret = ceph_lstat(cmount, "/", &st);
    if (ret) {
        DdmPrintW(L"GetDiskFreeSpace ceph_lstat return : %d\n", ret);
        return -1;
    }
    
    ULONGLONG used =  st.st_size;
    static ULONGLONG  max_size = 0;
    if (max_size == 0) {

        WCHAR filepath[MAX_PATH_CEPH] = { 0 };
        GetAbsPath(filepath, MAX_PATH_CEPH, L"");

        ctx_common request;
        ZeroMemory(&request, sizeof(request));
        request.isDirectory = DokanFileInfo->IsDirectory;
        wchar_to_char(request.path, filepath, MAX_PATH_CEPH);
        HANDLE h = PipeConnect(CEPH_CHANNEL_NAME);
        if (SendDataToUI2(h, DDM_CAPACITY, (void*)&request, sizeof(request)) == sizeof(request)) {
            pDdm_msg_ret pResult = (pDdm_msg_ret)malloc(sizeof(ddm_message) + sizeof(ddm_capacity));
            ReadDataFromUI2(h, (void*)pResult, sizeof(ddm_message) + sizeof(ddm_capacity));
            if (pResult->ctxLength == 0) {
                DdmPrintW(L"WinCephGetDiskFreeSpace Server Respone REJECT.\n");
                free(pResult);
                return -1;
            }
            pDdm_capacity pctx = (pDdm_capacity)pResult->ctx;
            max_size = pctx->total;
            CloseHandle(h);
            free(pResult);
        } else {
            DdmPrintW(L"WinCephGetDiskFreeSpace Send data error.\n");
            return -1;
        }
    }
    *TotalNumberOfBytes = max_size;
    *FreeBytesAvailable = (max_size - used) < 0 ? 0 : max_size - used;
    *TotalNumberOfFreeBytes = *FreeBytesAvailable;
    return 0;
}


static int
WinCephUnmount(
    PDOKAN_FILE_INFO    DokanFileInfo)
{
    DbgPrintW(L"Unmount\n");
    fwprintf(stderr, L"umount\n");
    ceph_unmount(cmount);
    return 0;
}


static void unmount_atexit() 
{
    int ret = ceph_unmount(cmount);
    DdmPrintW(L"umount FINISHED [%d]\n", ret);
    return;
}

int __cdecl
//wmain(ULONG argc, PWCHAR argv[])
main(int argc, char* argv[])
{
    printf("sizeof(DWORD) is [%d]\n", sizeof(DWORD));
    printf("sizeof(WCHAR) is [%d]\n", sizeof(WCHAR));
    printf("sizeof(ULONG) is [%d]\n", sizeof(ULONG));
    printf("sizeof(LPWSTR) is [%d]\n", sizeof(LPWSTR));
    printf("sizeof(LPCVOID) is [%d]\n", sizeof(LPCVOID));
    printf("sizeof(HANDLE) is [%d]\n", sizeof(HANDLE));
    printf("sizeof(dirent) is %d\n", sizeof(struct dirent));
    printf("sizeof(short) is %d\n", sizeof(short));
    printf("sizeof(int) is %d\n", sizeof(int));
    printf("sizeof(long) is %d\n", sizeof(long));
    
    char sub_mount_path[4096];
    strcpy(sub_mount_path, "/");
    
    char msg[MAX_PATH_CEPH];
    int status;
    ULONG command;
    PDOKAN_OPERATIONS dokanOperations =
            (PDOKAN_OPERATIONS)malloc(sizeof(DOKAN_OPERATIONS));
    PDOKAN_OPTIONS dokanOptions =
            (PDOKAN_OPTIONS)malloc(sizeof(DOKAN_OPTIONS));

    if(argc==2)
    {
        if(strcmp(argv[1], "--version")==0 || strcmp(argv[1], "-v")==0)
        {
            ceph_show_version();
            return 0;
        }
    }

    if (argc < 5) {
        ceph_show_version();
        fprintf(stderr, "ceph-dokan.exe\n"
            "  -c CephConfFile  (ex. /r c:\\ceph.conf)\n"
            "  -l DriveLetter (ex. /l m)\n"
            "  -t ThreadCount (ex. /t 5)\n"
            "  -d (enable debug output)\n"
            "  -s (use stderr for output)\n"
            "  -m (use removable drive)\n"
            "  -u Uid (use uid)\n"
            "  -g Gid (use gid)\n"
            "  -a (use posix acl)\n"
            "  -x sub_mount_path\n"
            );
        return -1;
    }
    
    //ceph_show_version();
    g_DebugMode = FALSE;
    g_UseStdErr = FALSE;

    ZeroMemory(dokanOptions, sizeof(DOKAN_OPTIONS));
    dokanOptions->Version = DOKAN_VERSION;
    dokanOptions->ThreadCount = 5;

    WCHAR wargv[32][512];
    for (command = 0; command < argc; command++) {
        MultiByteToWideChar(CP_UTF8, 0, (LPCTSTR)argv[command], -1, wargv[command], 512);
        //fprintf(stderr,  "argv command:[%d] %s\n",command, argv[command]);
        fwprintf(stderr, L"argv command:[%d] %s\n",command, wargv[command]);
    }
    
    wcscpy(Wargv0, wargv[0]);

    for (command = 1; command < argc; command++) {
        switch (towlower(wargv[command][1])) {
        case L'c':
            command++;
            strcpy(ceph_conf_file, argv[command]);
            wcscpy(Wceph_conf_file, wargv[command]);
            DbgPrintW(L"ceph_conf_file: %s\n", ceph_conf_file);
            break;
        case L'l':
            command++;
            //wcscpy_s(MountPoint, sizeof(MountPoint)/sizeof(WCHAR), argv[command]);
            wcscpy(MountPoint, wargv[command]);
            dokanOptions->MountPoint = MountPoint;
            break;
        case L't':
            command++;
            dokanOptions->ThreadCount = (USHORT)_wtoi(wargv[command]);
            break;
        case L'd':
            g_DebugMode = TRUE;
            fwprintf(stderr, L"g_DebugMode = TRUE\n");
            break;
        case L's':
            g_UseStdErr = TRUE;
            fwprintf(stderr, L"g_UseStdErr = TRUE\n");
            break;
        case L'm':
            dokanOptions->Options |= DOKAN_OPTION_REMOVABLE;
            break;
        case L'u':
            command++;
            g_UID = (USHORT)_wtoi(wargv[command]);
            break;
        case L'g':
            command++;
            g_GID = (USHORT)_wtoi(wargv[command]);
            break;
        case L'a':
            g_UseACL = TRUE;
            break;
        case L'x':
            command++;
            strcpy(sub_mount_path, argv[command]);
            break;
        case L'n':
            command++;
            strcpy(LetterName, argv[command]);
            //char_to_wchar(LetterName, argv[command], sizeof(LetterName));
            break;
        default:
            fwprintf(stderr, L"unknown command: %s\n", wargv[command]);
            return -1;
        }
    }
    
    if (g_DebugMode) {
        dokanOptions->Options |= DOKAN_OPTION_DEBUG;
    }
    if (g_UseStdErr) {
        dokanOptions->Options |= DOKAN_OPTION_STDERR;
    }

    dokanOptions->Options |= DOKAN_OPTION_KEEP_ALIVE;

    ZeroMemory(dokanOperations, sizeof(DOKAN_OPERATIONS));
    dokanOperations->CreateFile = WinCephCreateFile;
    dokanOperations->OpenDirectory = WinCephOpenDirectory;
    dokanOperations->CreateDirectory = WinCephCreateDirectory;
    dokanOperations->Cleanup = WinCephCleanup;
    dokanOperations->CloseFile = WinCephCloseFile;
    dokanOperations->ReadFile = WinCephReadFile;
    dokanOperations->WriteFile = WinCephWriteFile;
    dokanOperations->FlushFileBuffers = WinCephFlushFileBuffers;
    dokanOperations->GetFileInformation = WinCephGetFileInformation;
    dokanOperations->FindFiles = WinCephFindFiles;
    dokanOperations->FindFilesWithPattern = NULL;
    dokanOperations->SetFileAttributes = WinCephSetFileAttributes;
    dokanOperations->SetFileTime = WinCephSetFileTime;
    dokanOperations->DeleteFile = WinCephDeleteFile;
    dokanOperations->DeleteDirectory = WinCephDeleteDirectory;
    dokanOperations->MoveFile = WinCephMoveFile;
    dokanOperations->SetEndOfFile = WinCephSetEndOfFile;
    dokanOperations->SetAllocationSize = WinCephSetAllocationSize;    
    dokanOperations->LockFile = WinCephLockFile;
    dokanOperations->UnlockFile = WinCephUnlockFile;
    dokanOperations->GetFileSecurity = WinCephGetFileSecurity;
    dokanOperations->SetFileSecurity = WinCephSetFileSecurity;
    dokanOperations->GetVolumeInformation = WinCephGetVolumeInformation;
    dokanOperations->Unmount = WinCephUnmount;
    dokanOperations->GetDiskFreeSpace = WinCephGetDiskFreeSpace;

    //init socket
    WORD VerNum = MAKEWORD(2, 2);
    WSADATA VerData;
    if (WSAStartup(VerNum, &VerData) != 0) {
     // DdmPrintW("FAILED to init winsock!!!");
      return -1;
    }
    
    //ceph_mount
    int ret = 0;
    ceph_create(&cmount, NULL);
    ret = ceph_conf_read_file(cmount, ceph_conf_file);
    if(ret)
    {
        DdmPrintW(L"ceph_conf_read_file error!");
        return ret;
    }

    ret = ceph_mount(cmount, sub_mount_path);
    if(ret)
    {
        DdmPrintW(L"ceph_mount return code: %d", ret);
        return ret;
    }
    // if (!PipeConnect()) {
    //     DdmPrintW(L"PipeConnect Error\n");
    //     return -1;
    // }
    
    atexit(unmount_atexit);
    struct pipe_io_op io;
    io.abort = ServerAbort;
    RegisterIO(&io);
    //初始化密钥KEY
    SM4_init_key(_key16);
    HANDLE hServer =  PipeConnect(CEPH_CHANNEL_NAME);
    ListServerState(hServer);

    DdmPrintW(L"ceph_mnt OK\n");
    status = DokanMain(dokanOptions, dokanOperations);
    DdmPrintW(L"DokanMain Return Code:%d\n", status);
    PipeClose(hServer);
    free(dokanOptions);
    free(dokanOperations);
    return 0;
}

