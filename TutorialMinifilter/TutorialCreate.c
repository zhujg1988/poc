/*++

Module Name:
    TutorialCreate.c

Abstract:
    教学版透明加解密驱动 - 文件创建/打开处理
    
    功能：
    1. PreCreate: 检查文件是否需要监控
    2. PostCreate: 检查文件是否已加密（读取魔术字节）

--*/

#include "Tutorial.h"

FLT_PREOP_CALLBACK_STATUS
TutorialPreCreate(
    _Inout_ PFLT_CALLBACK_DATA Data,
    _In_ PCFLT_RELATED_OBJECTS FltObjects,
    _Flt_CompletionContext_Outptr_ PVOID *CompletionContext
)
/*++

功能说明:
    文件创建/打开操作的Pre回调
    
    在这里我们可以：
    1. 检查文件名和扩展名
    2. 决定是否需要监控这个文件
    3. 决定是否需要PostCreate回调
    
参数:
    Data - I/O请求数据
    FltObjects - 过滤器相关对象
    CompletionContext - 传递给PostCreate的上下文

返回值:
    FLT_PREOP_SUCCESS_WITH_CALLBACK - 需要PostCreate回调
    FLT_PREOP_SUCCESS_NO_CALLBACK - 不需要PostCreate回调

--*/
{
    PFLT_FILE_NAME_INFORMATION nameInfo = NULL;
    NTSTATUS status;
    BOOLEAN isMonitored = FALSE;

    UNREFERENCED_PARAMETER(FltObjects);
    UNREFERENCED_PARAMETER(CompletionContext);

    //
    // 步骤1: 获取文件名信息
    //
    status = FltGetFileNameInformation(
        Data,
        FLT_FILE_NAME_NORMALIZED | FLT_FILE_NAME_QUERY_DEFAULT,
        &nameInfo
    );

    if (!NT_SUCCESS(status)) {
        // 无法获取文件名，不监控
        return FLT_PREOP_SUCCESS_NO_CALLBACK;
    }

    //
    // 步骤2: 解析文件名
    // 将完整路径分解为：卷、路径、文件名、扩展名
    //
    status = FltParseFileNameInformation(nameInfo);
    if (!NT_SUCCESS(status)) {
        FltReleaseFileNameInformation(nameInfo);
        return FLT_PREOP_SUCCESS_NO_CALLBACK;
    }

    TutorialDbgPrint("PreCreate: 文件 = %wZ\n", &nameInfo->Name);

    //
    // 步骤3: 检查是否为目标文件类型
    //
    isMonitored = TutorialIsMonitoredFile(&nameInfo->Name);

    FltReleaseFileNameInformation(nameInfo);

    if (!isMonitored) {
        // 不是监控的文件类型，不需要后续处理
        TutorialDbgPrint("PreCreate: 非监控文件，跳过\n");
        return FLT_PREOP_SUCCESS_NO_CALLBACK;
    }

    TutorialDbgPrint("PreCreate: 监控文件，需要PostCreate检查加密状态\n");

    //
    // 是监控的文件，需要PostCreate回调来检查加密状态
    //
    return FLT_PREOP_SUCCESS_WITH_CALLBACK;
}

FLT_POSTOP_CALLBACK_STATUS
TutorialPostCreate(
    _Inout_ PFLT_CALLBACK_DATA Data,
    _In_ PCFLT_RELATED_OBJECTS FltObjects,
    _In_opt_ PVOID CompletionContext,
    _In_ FLT_POST_OPERATION_FLAGS Flags
)
/*++

功能说明:
    文件创建/打开操作的Post回调
    
    关键任务：
    1. 创建文件上下文
    2. 读取文件开头，检查是否有加密标识
    3. 设置文件上下文的IsEncrypted标志
    
    为什么在Post而不是Pre？
    - Pre时文件还未打开，无法读取内容
    - Post时文件已打开，可以读取文件头

参数:
    Data - I/O请求数据
    FltObjects - 过滤器相关对象
    CompletionContext - 从PreCreate传递的上下文
    Flags - 后处理标志

返回值:
    FLT_POSTOP_FINISHED_PROCESSING - 处理完成

--*/
{
    NTSTATUS status;
    PTUTORIAL_FILE_CONTEXT fileContext = NULL;
    LARGE_INTEGER byteOffset;
    ULONG bytesRead;
    UCHAR magicBuffer[TUTORIAL_MAGIC_SIZE];

    UNREFERENCED_PARAMETER(CompletionContext);
    UNREFERENCED_PARAMETER(Flags);

    //
    // 步骤1: 检查Create操作是否成功
    //
    if (!NT_SUCCESS(Data->IoStatus.Status) ||
        (Data->IoStatus.Status == STATUS_REPARSE)) {
        // Create失败或重解析，无需处理
        return FLT_POSTOP_FINISHED_PROCESSING;
    }

    TutorialDbgPrint("PostCreate: 文件打开成功\n");

    //
    // 步骤2: 创建或获取文件上下文
    //
    // 文件上下文的生命周期：
    // - 文件打开时创建
    // - 文件关闭时自动释放
    // - 用于存储文件的加密状态等信息
    //
    status = FltAllocateContext(
        FltObjects->Filter,
        FLT_STREAMHANDLE_CONTEXT,
        sizeof(TUTORIAL_FILE_CONTEXT),
        NonPagedPool,
        &fileContext
    );

    if (!NT_SUCCESS(status)) {
        TutorialDbgPrint("PostCreate: 分配上下文失败, status = 0x%08X\n", status);
        return FLT_POSTOP_FINISHED_PROCESSING;
    }

    //
    // 步骤3: 初始化文件上下文
    //
    RtlZeroMemory(fileContext, sizeof(TUTORIAL_FILE_CONTEXT));
    fileContext->IsEncrypted = FALSE;
    fileContext->IsMonitored = TRUE;

    //
    // 步骤4: 读取文件开头，检查加密标识
    //
    // 文件布局（已加密）:
    // [0-7字节]: "TUTENC!!" (魔术字节)
    // [8-N字节]: 加密数据
    //
    byteOffset.QuadPart = 0;  // 从文件开头读取

    status = FltReadFile(
        FltObjects->Instance,
        FltObjects->FileObject,
        &byteOffset,
        TUTORIAL_MAGIC_SIZE,
        magicBuffer,
        FLTFL_IO_OPERATION_NON_CACHED,
        &bytesRead,
        NULL,
        NULL
    );

    if (NT_SUCCESS(status) && bytesRead == TUTORIAL_MAGIC_SIZE) {
        //
        // 检查魔术字节
        //
        if (RtlCompareMemory(magicBuffer, TUTORIAL_MAGIC, TUTORIAL_MAGIC_SIZE) == 
            TUTORIAL_MAGIC_SIZE) {
            
            // 文件已加密！
            fileContext->IsEncrypted = TRUE;
            TutorialDbgPrint("PostCreate: 检测到加密标识，文件已加密\n");
        }
        else {
            TutorialDbgPrint("PostCreate: 无加密标识，文件未加密\n");
        }
    }
    else {
        // 读取失败或文件太小，当作未加密
        TutorialDbgPrint("PostCreate: 无法读取文件头（可能是新文件）\n");
    }

    //
    // 步骤5: 设置文件上下文
    //
    status = FltSetStreamHandleContext(
        FltObjects->Instance,
        FltObjects->FileObject,
        FLT_SET_CONTEXT_REPLACE_IF_EXISTS,
        fileContext,
        NULL
    );

    if (!NT_SUCCESS(status)) {
        TutorialDbgPrint("PostCreate: 设置上下文失败, status = 0x%08X\n", status);
    }

    //
    // 释放上下文引用
    // 注意：这不会删除上下文，只是减少引用计数
    // 文件关闭时系统会自动删除上下文
    //
    FltReleaseContext(fileContext);

    return FLT_POSTOP_FINISHED_PROCESSING;
}
