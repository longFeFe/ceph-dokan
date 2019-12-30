#pragma once
#pragma pack(1)
#include <time.h>
#ifdef _WIN32
#include <windows.h>
#define CEPH_CHANNEL_NAME "\\\\.\\pipe\\shutupandtakemymoney.2019.09.09\\"
#else 
#include <iostream>
#define CEPH_CHANNEL_NAME  "/tmp/socket_vdisk"
#define MAX_PATH 260
#endif

//行为定义
enum DDMAction {
	DDM_DELETE, // from vdisk
	DDM_MOVE, //  from vdisk
	DDM_CREATE, //from vdisk
	DDM_OPEN, //from hook
	DDM_CAPACITY,//
	DDM_READDIR, //
	DDM_READFILEINFO,//
	DDM_MOUNT,
	DDM_UNMOUNT,
	
};

//放行规则
enum DDMActionResult {
	RET_UNDO, //撤销当前行为
	RET_ALLOW,//允许通过
	RET_FORBID, //禁止
};


//权限位
#define AUTH_BASE_LINE  1
#define DDM_READ  (AUTH_BASE_LINE << 1)
#define DDM_WRITE (AUTH_BASE_LINE << 2)
#define DDM_RW	  (DDM_READ | DDM_WRITE)



typedef struct _DDM_Content {
		char path[MAX_PATH];
		int	 isDirectory;	//0文件， 1目录
} ctx_open, ctx_delete, 
ctx_enable_write, ctx_create, ctx_common;

typedef struct _DDM_Content_Reanme {
		char pathOld[MAX_PATH];
		char pathNew[MAX_PATH];
		int	 isDirectory;	//0文件， 1目录
} ctx_rename, ctx_cut, ctx_move, ctx_copy;




typedef struct _DDM_Message {
		unsigned int 	pid;
		unsigned int 	ctxLength; // length of ctx
		int				action;
		char			ctx[0];
} ddm_message, *pDdm_message;


//文件信息
typedef struct  _DDM_Fileinfo {
	char name[MAX_PATH];
	time_t ctime;
	time_t mtime;
	time_t atime;
	unsigned long long  size; //byte
	unsigned short  mode; //权限
	int 	nlink;
	int isDirectory; //0 = file , 1 = directory
} ddm_fileInfo, *pDdm_fileInfo;


//容量
typedef struct  _DDM_Capacity {  
	unsigned long long  total; //总共 字节
	unsigned long long  used; //已使用 字节
}ddm_capacity, *pDdm_capacity;


//返回结果
typedef struct _DDM_MessageResult {
	int				result; //action的结果
	unsigned int 	ctxLength; // length of ctx
	char			ctx[0];
}ddm_msg_ret, *pDdm_msg_ret;
