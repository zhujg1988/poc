/*++

Module Name:
    TutorialRead.c

Abstract:
    教学版透明加解密驱动 - 文件读取处理
    
    核心功能：
    1. PreRead: 对授权进程，替换缓冲区为驱动分配的缓冲区
    2. PostRead: 解密数据，复制回应用程序缓冲区
    
    透明解密的关键实现

--*/

#include "Tutorial.h"

FLT_PREOP_CALLBACK_STATUS
TutorialPreRead(
    _Inout_ PFLT_CALLBACK_DATA Data,
    _In_ PCFLT_RELATED_OBJECTS FltObjects,
    _Flt_CompletionContext_Outptr_ PVOID *CompletionContext
)
/*++

功能说明:
    文件读取的Pre回调 - 透明解密的第一步
    
    核心逻辑：
    1. 检查是否需要解密（授权进程 + 文件已加密）
    2. 分配新缓冲区
    3. 替换IRP的缓冲区指针
    4. 保存原始缓冲区地址，传递给PostRead
    
    为什么要替换缓冲区？
    - 文件系统会读取密文到IRP的缓冲区
    - 如果直接用应用程序的缓冲区，应用程序会看到密文
    - 我们用新缓冲区接收密文，在PostRead解密后再复制给应用程序

参数:
    Data - I/O请求数据
    FltObjects - 过滤器相关对象
    CompletionContext - 输出：传递给PostRead的上下文

返回值:
    FLT_PREOP_SUCCESS_WITH_CALLBACK - 需要PostRead解密
    FLT_PREOP_SUCCESS_NO_CALLBACK - 不需要PostRead

--*/
{
    PTUTORIAL_FILE_CONTEXT fileContext = NULL;
    PTUTORIAL_BUFFER_SWAP_CONTEXT swapContext = NULL;
    NTSTATUS status;
    PVOID newBuffer = NULL;
    PMDL newMdl = NULL;
    ULONG readLength;

    *CompletionContext = NULL;

    //
    // 步骤1: 获取文件上下文
    //
    status = FltGetStreamHandleContext(
        FltObjects->Instance,
        FltObjects->FileObject,
        &fileContext
    );

    if (!NT_SUCCESS(status) || fileContext == NULL) {
        // 没有上下文，说明不是监控的文件
        return FLT_PREOP_SUCCESS_NO_CALLBACK;
    }

    //
    // 步骤2: 检查文件是否已加密
    //
    if (!fileContext->IsEncrypted) {
        // 文件未加密，不需要解密
        TutorialDbgPrint("PreRead: 文件未加密，直接返回\n");
        FltReleaseContext(fileContext);
        return FLT_PREOP_SUCCESS_NO_CALLBACK;
    }

    //
    // 步骤3: 检查当前进程是否授权
    //
    if (!TutorialIsAuthorizedProcess()) {
        // 非授权进程读取已加密文件 → 返回密文（乱码）
        TutorialDbgPrint("PreRead: 非授权进程，返回密文\n");
        FltReleaseContext(fileContext);
        return FLT_PREOP_SUCCESS_NO_CALLBACK;
    }

    TutorialDbgPrint("PreRead: 授权进程读取加密文件，准备解密\n");

    //
    // 步骤4: 获取读取长度
    //
    readLength = Data->Iopb->Parameters.Read.Length;
    
    if (readLength == 0) {
        FltReleaseContext(fileContext);
        return FLT_PREOP_SUCCESS_NO_CALLBACK;
    }

    TutorialDbgPrint("PreRead: 读取长度 = %u 字节\n", readLength);

    //
    // 步骤5: 分配新缓冲区
    // 这个缓冲区用于接收从磁盘读取的密文
    //
    newBuffer = ExAllocatePoolWithTag(
        NonPagedPool,
        readLength,
        TUTORIAL_TAG
    );

    if (newBuffer == NULL) {
        TutorialDbgPrint("PreRead: 分配缓冲区失败\n");
        FltReleaseContext(fileContext);
        return FLT_PREOP_SUCCESS_NO_CALLBACK;
    }

    //
    // 步骤6: 如果需要，分配MDL
    // MDL (Memory Descriptor List) 用于描述内存缓冲区
    //
    if (FlagOn(Data->Iopb->MinorFunction, IRP_MN_MDL)) {
        newMdl = IoAllocateMdl(
            newBuffer,
            readLength,
            FALSE,
            FALSE,
            NULL
        );

        if (newMdl == NULL) {
            TutorialDbgPrint("PreRead: 分配MDL失败\n");
            SAFE_FREE(newBuffer, TUTORIAL_TAG);
            FltReleaseContext(fileContext);
            return FLT_PREOP_SUCCESS_NO_CALLBACK;
        }

        MmBuildMdlForNonPagedPool(newMdl);
    }

    //
    // 步骤7: 创建缓冲区交换上下文
    // 用于在Pre和Post之间传递信息
    //
    swapContext = ExAllocatePoolWithTag(
        NonPagedPool,
        sizeof(TUTORIAL_BUFFER_SWAP_CONTEXT),
        TUTORIAL_TAG
    );

    if (swapContext == NULL) {
        TutorialDbgPrint("PreRead: 分配交换上下文失败\n");
        SAFE_FREE_MDL(newMdl);
        SAFE_FREE(newBuffer, TUTORIAL_TAG);
        FltReleaseContext(fileContext);
        return FLT_PREOP_SUCCESS_NO_CALLBACK;
    }

    //
    // 步骤8: 保存原始缓冲区信息
    //
    swapContext->OriginalBuffer = Data->Iopb->Parameters.Read.ReadBuffer;
    swapContext->OriginalMdl = Data->Iopb->Parameters.Read.MdlAddress;
    swapContext->SwappedBuffer = newBuffer;
    swapContext->SwappedMdl = newMdl;
    swapContext->BufferSize = readLength;

    TutorialDbgPrint("PreRead: 原始缓冲区 = %p, 新缓冲区 = %p\n",
        swapContext->OriginalBuffer, newBuffer);

    //
    // 步骤9: 替换IRP的缓冲区
    // 关键！文件系统会读取密文到这个新缓冲区
    //
    Data->Iopb->Parameters.Read.ReadBuffer = newBuffer;
    Data->Iopb->Parameters.Read.MdlAddress = newMdl;

    //
    // 设置标志：告诉系统我们修改了缓冲区
    //
    FltSetCallbackDataDirty(Data);

    //
    // 步骤10: 传递上下文给PostRead
    //
    *CompletionContext = swapContext;

    FltReleaseContext(fileContext);

    TutorialDbgPrint("PreRead: 缓冲区替换完成，等待PostRead解密\n");

    //
    // 返回：需要PostRead回调
    //
    return FLT_PREOP_SUCCESS_WITH_CALLBACK;
}

FLT_POSTOP_CALLBACK_STATUS
TutorialPostRead(
    _Inout_ PFLT_CALLBACK_DATA Data,
    _In_ PCFLT_RELATED_OBJECTS FltObjects,
    _In_opt_ PVOID CompletionContext,
    _In_ FLT_POST_OPERATION_FLAGS Flags
)
/*++

功能说明:
    文件读取的Post回调 - 透明解密的第二步
    
    核心逻辑：
    1. 从新缓冲区获取密文
    2. 解密（XOR）
    3. 复制明文到应用程序缓冲区
    4. 清理资源
    
    这时：
    - 文件系统已经从磁盘读取密文到新缓冲区
    - 我们解密后复制给应用程序
    - 应用程序看到的是明文

参数:
    Data - I/O请求数据
    FltObjects - 过滤器相关对象
    CompletionContext - 从PreRead传递的上下文
    Flags - 后处理标志

返回值:
    FLT_POSTOP_FINISHED_PROCESSING - 处理完成

--*/
{
    PTUTORIAL_BUFFER_SWAP_CONTEXT swapContext = NULL;
    PUCHAR readBuffer = NULL;
    ULONG bytesRead;

    UNREFERENCED_PARAMETER(FltObjects);
    UNREFERENCED_PARAMETER(Flags);

    swapContext = (PTUTORIAL_BUFFER_SWAP_CONTEXT)CompletionContext;

    //
    // 步骤1: 检查上下文和读取状态
    //
    if (swapContext == NULL) {
        return FLT_POSTOP_FINISHED_PROCESSING;
    }

    if (!NT_SUCCESS(Data->IoStatus.Status)) {
        // 读取失败，清理资源
        TutorialDbgPrint("PostRead: 读取失败, status = 0x%08X\n", 
            Data->IoStatus.Status);
        goto Cleanup;
    }

    bytesRead = (ULONG)Data->IoStatus.Information;
    
    if (bytesRead == 0) {
        TutorialDbgPrint("PostRead: 读取0字节\n");
        goto Cleanup;
    }

    TutorialDbgPrint("PostRead: 成功读取 %u 字节密文\n", bytesRead);

    //
    // 步骤2: 获取缓冲区地址
    //
    if (swapContext->SwappedMdl != NULL) {
        // 使用MDL映射的缓冲区
        readBuffer = MmGetSystemAddressForMdlSafe(
            swapContext->SwappedMdl,
            NormalPagePriority
        );
    }
    else {
        // 直接使用缓冲区
        readBuffer = swapContext->SwappedBuffer;
    }

    if (readBuffer == NULL) {
        TutorialDbgPrint("PostRead: 获取缓冲区地址失败\n");
        goto Cleanup;
    }

    //
    // 步骤3: 解密数据
    //
    // 跳过文件头的魔术字节（如果有）
    // 如果读取偏移为0，跳过前8字节的"TUTENC!!"标识
    //
    if (Data->Iopb->Parameters.Read.ByteOffset.QuadPart == 0 &&
        bytesRead > TUTORIAL_MAGIC_SIZE) {
        
        // 只解密魔术字节之后的数据
        TutorialXorEncryptDecrypt(
            readBuffer + TUTORIAL_MAGIC_SIZE,
            bytesRead - TUTORIAL_MAGIC_SIZE,
            TUTORIAL_XOR_KEY
        );
        
        TutorialDbgPrint("PostRead: 解密 %u 字节（跳过魔术字节）\n",
            bytesRead - TUTORIAL_MAGIC_SIZE);
    }
    else {
        // 解密全部数据
        TutorialXorEncryptDecrypt(
            readBuffer,
            bytesRead,
            TUTORIAL_XOR_KEY
        );
        
        TutorialDbgPrint("PostRead: 解密 %u 字节\n", bytesRead);
    }

    //
    // 步骤4: 复制明文到应用程序缓冲区
    //
    __try {
        RtlCopyMemory(
            swapContext->OriginalBuffer,
            readBuffer,
            bytesRead
        );
        
        TutorialDbgPrint("PostRead: 复制明文到应用程序缓冲区\n");
    }
    __except(EXCEPTION_EXECUTE_HANDLER) {
        TutorialDbgPrint("PostRead: 复制数据时发生异常\n");
        Data->IoStatus.Status = STATUS_INVALID_USER_BUFFER;
        Data->IoStatus.Information = 0;
    }

Cleanup:
    //
    // 步骤5: 清理资源
    //
    if (swapContext != NULL) {
        // 释放MDL
        SAFE_FREE_MDL(swapContext->SwappedMdl);
        
        // 释放缓冲区
        SAFE_FREE(swapContext->SwappedBuffer, TUTORIAL_TAG);
        
        // 释放交换上下文
        SAFE_FREE(swapContext, TUTORIAL_TAG);
    }

    TutorialDbgPrint("PostRead: 解密完成，资源已清理\n");

    return FLT_POSTOP_FINISHED_PROCESSING;
}
