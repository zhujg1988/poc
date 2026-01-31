/*++

Module Name:
    TutorialDriver.c

Abstract:
    教学版透明加解密驱动 - 主文件
    
    功能：
    1. 驱动初始化和注册
    2. Minifilter框架集成
    3. 回调函数注册

Author:
    Tutorial Version - Simplified for educational purposes

Environment:
    Kernel mode

--*/

#include "Tutorial.h"

//
// 全局变量
//

// 过滤器句柄 - 代表我们的minifilter驱动实例
PFLT_FILTER gFilterHandle = NULL;

//
// 操作回调注册表
// 定义了我们要拦截哪些I/O操作
//

CONST FLT_OPERATION_REGISTRATION Callbacks[] = {

    // 拦截文件创建/打开操作
    // 用于检查文件是否已加密，以及是否需要监控
    {
        IRP_MJ_CREATE,                    // 主功能码：创建/打开
        0,                                // 标志
        TutorialPreCreate,                // Pre回调
        TutorialPostCreate                // Post回调
    },

    // 拦截文件读取操作
    // 用于对授权进程透明解密
    {
        IRP_MJ_READ,                      // 主功能码：读取
        0,                                // 标志
        TutorialPreRead,                  // Pre回调
        TutorialPostRead                  // Post回调
    },

    // 拦截文件写入操作
    // 用于对授权进程透明加密
    {
        IRP_MJ_WRITE,                     // 主功能码：写入
        0,                                // 标志
        TutorialPreWrite,                 // Pre回调
        TutorialPostWrite                 // Post回调
    },

    // 结束标记
    { IRP_MJ_OPERATION_END }
};

//
// 上下文注册
// 定义我们使用的上下文类型
//

CONST FLT_CONTEXT_REGISTRATION ContextRegistration[] = {
    
    // 文件流上下文 - 用于存储每个文件的加密状态
    {
        FLT_STREAMHANDLE_CONTEXT,         // 上下文类型
        0,                                // 标志
        NULL,                             // 清理回调
        sizeof(TUTORIAL_FILE_CONTEXT),    // 大小
        TUTORIAL_TAG                      // 池标记
    },

    // 结束标记
    { FLT_CONTEXT_END }
};

//
// 过滤器注册结构
// 这是minifilter的"名片"，告诉系统我们的驱动信息
//

CONST FLT_REGISTRATION FilterRegistration = {

    sizeof(FLT_REGISTRATION),         // 结构大小
    FLT_REGISTRATION_VERSION,         // 版本
    0,                                // 标志

    ContextRegistration,              // 上下文注册
    Callbacks,                        // 操作回调

    TutorialUnload,                   // 卸载函数

    TutorialInstanceSetup,            // 实例设置
    NULL,                             // 实例查询卸载
    NULL,                             // 实例卸载开始
    NULL,                             // 实例卸载完成

    NULL,                             // 生成文件名
    NULL,                             // 生成目标文件名
    NULL                              // 规范化名称组件
};

//
// 驱动入口点
//

NTSTATUS
DriverEntry(
    _In_ PDRIVER_OBJECT DriverObject,
    _In_ PUNICODE_STRING RegistryPath
)
/*++

功能说明:
    驱动程序的入口点，类似于用户态程序的main函数
    
    执行流程：
    1. 向Filter Manager注册我们的minifilter
    2. 开始过滤I/O操作
    
参数:
    DriverObject - 系统创建的驱动对象
    RegistryPath - 驱动在注册表中的路径

返回值:
    STATUS_SUCCESS - 成功
    其他值 - 失败

--*/
{
    NTSTATUS status;

    UNREFERENCED_PARAMETER(RegistryPath);

    TutorialDbgPrint("DriverEntry: 教学版透明加解密驱动开始加载...\n");

    //
    // 步骤1: 向Filter Manager注册我们的过滤器
    //
    // FltRegisterFilter做了什么？
    // - 告诉系统我们的驱动信息（名称、版本、回调函数等）
    // - 创建过滤器对象
    // - 返回过滤器句柄，后续操作需要用到
    //
    status = FltRegisterFilter(
        DriverObject,           // 驱动对象
        &FilterRegistration,    // 注册信息
        &gFilterHandle          // 输出：过滤器句柄
    );

    if (!NT_SUCCESS(status)) {
        TutorialDbgPrint("DriverEntry: FltRegisterFilter失败, status = 0x%08X\n", status);
        return status;
    }

    TutorialDbgPrint("DriverEntry: 过滤器注册成功\n");

    //
    // 步骤2: 开始过滤I/O操作
    //
    // FltStartFiltering做了什么？
    // - 激活过滤器，开始拦截I/O请求
    // - 之后所有符合条件的I/O操作都会调用我们的回调函数
    //
    status = FltStartFiltering(gFilterHandle);

    if (!NT_SUCCESS(status)) {
        TutorialDbgPrint("DriverEntry: FltStartFiltering失败, status = 0x%08X\n", status);
        FltUnregisterFilter(gFilterHandle);
        return status;
    }

    TutorialDbgPrint("DriverEntry: 驱动加载成功，开始过滤I/O操作\n");
    TutorialDbgPrint("DriverEntry: 授权进程: notepad.exe\n");
    TutorialDbgPrint("DriverEntry: 监控文件类型: .txt\n");
    TutorialDbgPrint("DriverEntry: 加密算法: XOR (密钥=0x42)\n");

    return STATUS_SUCCESS;
}

//
// 驱动卸载函数
//

NTSTATUS
TutorialUnload(
    _In_ FLT_FILTER_UNLOAD_FLAGS Flags
)
/*++

功能说明:
    驱动卸载时调用
    清理资源，停止过滤

参数:
    Flags - 卸载标志

返回值:
    STATUS_SUCCESS - 允许卸载

--*/
{
    UNREFERENCED_PARAMETER(Flags);

    TutorialDbgPrint("TutorialUnload: 开始卸载驱动...\n");

    //
    // 注销过滤器
    // 这会：
    // 1. 停止所有I/O拦截
    // 2. 清理所有上下文
    // 3. 释放资源
    //
    if (gFilterHandle != NULL) {
        FltUnregisterFilter(gFilterHandle);
        gFilterHandle = NULL;
    }

    TutorialDbgPrint("TutorialUnload: 驱动卸载完成\n");

    return STATUS_SUCCESS;
}

//
// 实例设置回调
//

NTSTATUS
TutorialInstanceSetup(
    _In_ PCFLT_RELATED_OBJECTS FltObjects,
    _In_ FLT_INSTANCE_SETUP_FLAGS Flags,
    _In_ DEVICE_TYPE VolumeDeviceType,
    _In_ FLT_FILESYSTEM_TYPE VolumeFilesystemType
)
/*++

功能说明:
    当系统挂载新卷时调用
    我们可以决定是否要附加到这个卷
    
    教学版简化：附加到所有NTFS卷

参数:
    FltObjects - 过滤器相关对象
    Flags - 设置标志
    VolumeDeviceType - 卷设备类型
    VolumeFilesystemType - 文件系统类型

返回值:
    STATUS_SUCCESS - 附加到此卷
    STATUS_FLT_DO_NOT_ATTACH - 不附加

--*/
{
    UNREFERENCED_PARAMETER(FltObjects);
    UNREFERENCED_PARAMETER(Flags);
    UNREFERENCED_PARAMETER(VolumeDeviceType);

    TutorialDbgPrint("TutorialInstanceSetup: 检测到新卷\n");

    //
    // 只附加到NTFS文件系统
    // 教学版简化，实际产品可能需要支持更多文件系统
    //
    if (VolumeFilesystemType == FLT_FSTYPE_NTFS) {
        TutorialDbgPrint("TutorialInstanceSetup: NTFS卷，附加成功\n");
        return STATUS_SUCCESS;
    }

    TutorialDbgPrint("TutorialInstanceSetup: 非NTFS卷，跳过\n");
    return STATUS_FLT_DO_NOT_ATTACH;
}
