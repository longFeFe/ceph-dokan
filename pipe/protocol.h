#ifndef CEPHDOKAN_PIPE_PROTOCOL_H_
#define CEPHDOKAN_PIPE_PROTOCOL_H_
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
	DDM_DELETE, // from dokan
	DDM_MOVE, //  from dokan
	DDM_CREATE, //from dokan
	DDM_RENAME, //from dokan
	DDM_OPEN, 
	DDM_CAPACITY,//from dokan
	DDM_READDIR, //from dokan
	DDM_READFILEINFO,//from dokan
	DDM_FILESTATUS,//from dokan
	DDM_MODIFY,
	DDM_CLOSE,
	DDM_MOUNT, //
	DDM_UNMOUNT, //
	
};

//查询结果
enum DDMActionResult {
	RET_FAILED, //默认值
	RET_ALLOW,//允许通过
	RET_FORBID, //禁止
	RET_NOTFOUND,
};

//权限位
#define AUTH_BASE_LINE  0x0001
#define AUTH_READ  (AUTH_BASE_LINE << 0)
#define AUTH_WRITE (AUTH_BASE_LINE << 1)
#define AUTH_CREATE (AUTH_BASE_LINE << 2) //文件夹内是否允许创建

#define AUTH_RW	  (AUTH_READ | AUTH_WRITE)

#define AUTH_DOWNLOAD (AUTH_BASE_LINE << 3)
#define AUTH_LOCK (AUTH_BASE_LINE << 4)
#define AUTH_PRINT (AUTH_BASE_LINE << 5)
#define AUTH_DELETE (AUTH_BASE_LINE << 6)
#define AUTH_RENAME (AUTH_BASE_LINE << 7)
//管理权限
#define AUTH_ROOT (~AUTH_BASE_LINE | AUTH_BASE_LINE)





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
	unsigned long long ctime;
	unsigned long long mtime;
	unsigned long long atime;
	unsigned long long  size; //byte
	int 	nlink;
	int isDirectory; //0 = file , 1 = directory
	unsigned short  mode; //权限
	char name[MAX_PATH];
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
#endif