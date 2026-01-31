/*++

Module Name:
    Tutorial.h

Abstract:
    教学版透明加解密驱动 - 头文件
    简化的Minifilter驱动，用于演示文件透明加解密的核心原理

Environment:
    Kernel mode

--*/

#pragma once

#include <fltKernel.h>
#include <dontuse.h>
#include <suppress.h>

//
// 全局常量定义
//

// 内存池标记（用于调试和内存泄漏检测）
#define TUTORIAL_TAG 'TTUT'  // 'TUTT' reversed

// 文件加密标识（魔术字节）
#define TUTORIAL_MAGIC "TUTENC!!"  // 8字节标识
#define TUTORIAL_MAGIC_SIZE 8

// XOR加密密钥（简单演示用）
#define TUTORIAL_XOR_KEY 0x42

// 最大路径长度
#define TUTORIAL_MAX_PATH 260

//
// 结构体定义
//

// 文件上下文 - 存储每个文件的加密状态
typedef struct _TUTORIAL_FILE_CONTEXT {
    BOOLEAN IsEncrypted;     // 文件是否已加密
    BOOLEAN IsMonitored;     // 文件是否在监控范围内
} TUTORIAL_FILE_CONTEXT, *PTUTORIAL_FILE_CONTEXT;

// 缓冲区交换上下文 - 用于Pre/Post回调之间传递信息
typedef struct _TUTORIAL_BUFFER_SWAP_CONTEXT {
    PVOID OriginalBuffer;    // 原始缓冲区地址（应用程序的）
    PVOID SwappedBuffer;     // 交换后的缓冲区地址（驱动分配的）
    PMDL OriginalMdl;        // 原始MDL
    PMDL SwappedMdl;         // 交换后的MDL
    ULONG BufferSize;        // 缓冲区大小
} TUTORIAL_BUFFER_SWAP_CONTEXT, *PTUTORIAL_BUFFER_SWAP_CONTEXT;

//
// 全局变量声明
//

extern PFLT_FILTER gFilterHandle;

//
// 函数声明
//

//
// TutorialDriver.c - 驱动主文件
//

DRIVER_INITIALIZE DriverEntry;

NTSTATUS
DriverEntry(
    _In_ PDRIVER_OBJECT DriverObject,
    _In_ PUNICODE_STRING RegistryPath
);

NTSTATUS
TutorialUnload(
    _In_ FLT_FILTER_UNLOAD_FLAGS Flags
);

NTSTATUS
TutorialInstanceSetup(
    _In_ PCFLT_RELATED_OBJECTS FltObjects,
    _In_ FLT_INSTANCE_SETUP_FLAGS Flags,
    _In_ DEVICE_TYPE VolumeDeviceType,
    _In_ FLT_FILESYSTEM_TYPE VolumeFilesystemType
);

//
// TutorialCreate.c - 文件创建/打开处理
//

FLT_PREOP_CALLBACK_STATUS
TutorialPreCreate(
    _Inout_ PFLT_CALLBACK_DATA Data,
    _In_ PCFLT_RELATED_OBJECTS FltObjects,
    _Flt_CompletionContext_Outptr_ PVOID *CompletionContext
);

FLT_POSTOP_CALLBACK_STATUS
TutorialPostCreate(
    _Inout_ PFLT_CALLBACK_DATA Data,
    _In_ PCFLT_RELATED_OBJECTS FltObjects,
    _In_opt_ PVOID CompletionContext,
    _In_ FLT_POST_OPERATION_FLAGS Flags
);

//
// TutorialRead.c - 文件读取处理
//

FLT_PREOP_CALLBACK_STATUS
TutorialPreRead(
    _Inout_ PFLT_CALLBACK_DATA Data,
    _In_ PCFLT_RELATED_OBJECTS FltObjects,
    _Flt_CompletionContext_Outptr_ PVOID *CompletionContext
);

FLT_POSTOP_CALLBACK_STATUS
TutorialPostRead(
    _Inout_ PFLT_CALLBACK_DATA Data,
    _In_ PCFLT_RELATED_OBJECTS FltObjects,
    _In_opt_ PVOID CompletionContext,
    _In_ FLT_POST_OPERATION_FLAGS Flags
);

//
// TutorialWrite.c - 文件写入处理
//

FLT_PREOP_CALLBACK_STATUS
TutorialPreWrite(
    _Inout_ PFLT_CALLBACK_DATA Data,
    _In_ PCFLT_RELATED_OBJECTS FltObjects,
    _Flt_CompletionContext_Outptr_ PVOID *CompletionContext
);

FLT_POSTOP_CALLBACK_STATUS
TutorialPostWrite(
    _Inout_ PFLT_CALLBACK_DATA Data,
    _In_ PCFLT_RELATED_OBJECTS FltObjects,
    _In_opt_ PVOID CompletionContext,
    _In_ FLT_POST_OPERATION_FLAGS Flags
);

//
// TutorialCipher.c - 加密/解密实现
//

VOID
TutorialXorEncryptDecrypt(
    _Inout_ PUCHAR Buffer,
    _In_ ULONG Length,
    _In_ UCHAR Key
);

//
// TutorialConfig.c - 配置和工具函数
//

BOOLEAN
TutorialIsAuthorizedProcess(
    VOID
);

BOOLEAN
TutorialIsMonitoredFile(
    _In_ PUNICODE_STRING FileName
);

BOOLEAN
TutorialIsTargetExtension(
    _In_ PUNICODE_STRING FileName
);

//
// 辅助宏定义
//

// 调试输出宏
#if DBG
#define TutorialDbgPrint(format, ...) \
    DbgPrint("[Tutorial] " format, ##__VA_ARGS__)
#else
#define TutorialDbgPrint(format, ...)
#endif

// 检查指针有效性
#define VALID_PTR(ptr) ((ptr) != NULL)

// 安全释放内存
#define SAFE_FREE(ptr, tag) \
    if (VALID_PTR(ptr)) { \
        ExFreePoolWithTag(ptr, tag); \
        ptr = NULL; \
    }

// 安全释放MDL
#define SAFE_FREE_MDL(mdl) \
    if (VALID_PTR(mdl)) { \
        IoFreeMdl(mdl); \
        mdl = NULL; \
    }
