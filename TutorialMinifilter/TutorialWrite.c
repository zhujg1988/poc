/*++

Module Name:
    TutorialWrite.c

Abstract:
    教学版透明加解密驱动 - 文件写入处理
    
    核心功能：
    1. PreWrite: 对授权进程，加密数据并替换缓冲区
    2. PostWrite: 如果是新文件，写入加密标识

--*/

#include "Tutorial.h"

FLT_PREOP_CALLBACK_STATUS
TutorialPreWrite(
    _Inout_ PFLT_CALLBACK_DATA Data,
    _In_ PCFLT_RELATED_OBJECTS FltObjects,
    _Flt_CompletionContext_Outptr_ PVOID *CompletionContext
)
/*++

功能说明:
    文件写入的Pre回调 - 透明加密的实现
    
    核心逻辑：
    1. 检查当前进程是否授权
    2. 分配新缓冲区
    3. 复制明文数据
    4. 加密（XOR）
    5. 替换IRP的缓冲区指针
    
    流程：
    应用程序写入明文 → PreWrite加密 → 文件系统写密文到磁盘

参数:
    Data - I/O请求数据
    FltObjects - 过滤器相关对象
    CompletionContext - 输出：传递给PostWrite的上下文

返回值:
    FLT_PREOP_SUCCESS_WITH_CALLBACK - 需要PostWrite
    FLT_PREOP_SUCCESS_NO_CALLBACK - 不需要PostWrite
    FLT_PREOP_COMPLETE - 拒绝写入

--*/
{
    PTUTORIAL_FILE_CONTEXT fileContext = NULL;
    PTUTORIAL_BUFFER_SWAP_CONTEXT swapContext = NULL;
    NTSTATUS status;
    PVOID newBuffer = NULL;
    PVOID writeBuffer = NULL;
    PMDL newMdl = NULL;
    ULONG writeLength;
    LARGE_INTEGER writeOffset;

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
    // 步骤2: 检查当前进程是否授权
    //
    if (!TutorialIsAuthorizedProcess()) {
        // 非授权进程试图写入
        // 为了保护加密文件，拒绝写入
        TutorialDbgPrint("PreWrite: 非授权进程，拒绝写入\n");
        
        FltReleaseContext(fileContext);
        
        Data->IoStatus.Status = STATUS_ACCESS_DENIED;
        Data->IoStatus.Information = 0;
        
        return FLT_PREOP_COMPLETE;
    }

    TutorialDbgPrint("PreWrite: 授权进程写入，准备加密\n");

    //
    // 步骤3: 获取写入参数
    //
    writeLength = Data->Iopb->Parameters.Write.Length;
    writeOffset = Data->Iopb->Parameters.Write.ByteOffset;
    
    if (writeLength == 0) {
        FltReleaseContext(fileContext);
        return FLT_PREOP_SUCCESS_NO_CALLBACK;
    }

    TutorialDbgPrint("PreWrite: 写入长度 = %u 字节, 偏移 = %I64d\n",
        writeLength, writeOffset.QuadPart);

    //
    // 步骤4: 获取原始写入缓冲区（明文）
    //
    if (Data->Iopb->Parameters.Write.MdlAddress != NULL) {
        writeBuffer = MmGetSystemAddressForMdlSafe(
            Data->Iopb->Parameters.Write.MdlAddress,
            NormalPagePriority
        );
    }
    else {
        writeBuffer = Data->Iopb->Parameters.Write.WriteBuffer;
    }

    if (writeBuffer == NULL) {
        TutorialDbgPrint("PreWrite: 无法获取写入缓冲区\n");
        FltReleaseContext(fileContext);
        return FLT_PREOP_SUCCESS_NO_CALLBACK;
    }

    //
    // 步骤5: 分配新缓冲区（用于存放密文）
    //
    newBuffer = ExAllocatePoolWithTag(
        NonPagedPool,
        writeLength,
        TUTORIAL_TAG
    );

    if (newBuffer == NULL) {
        TutorialDbgPrint("PreWrite: 分配缓冲区失败\n");
        FltReleaseContext(fileContext);
        return FLT_PREOP_SUCCESS_NO_CALLBACK;
    }

    //
    // 步骤6: 复制明文数据到新缓冲区
    //
    __try {
        RtlCopyMemory(newBuffer, writeBuffer, writeLength);
    }
    __except(EXCEPTION_EXECUTE_HANDLER) {
        TutorialDbgPrint("PreWrite: 复制数据时发生异常\n");
        SAFE_FREE(newBuffer, TUTORIAL_TAG);
        FltReleaseContext(fileContext);
        
        Data->IoStatus.Status = STATUS_INVALID_USER_BUFFER;
        Data->IoStatus.Information = 0;
        
        return FLT_PREOP_COMPLETE;
    }

    //
    // 步骤7: 加密数据
    //
    TutorialXorEncryptDecrypt(
        (PUCHAR)newBuffer,
        writeLength,
        TUTORIAL_XOR_KEY
    );

    TutorialDbgPrint("PreWrite: 加密 %u 字节完成\n", writeLength);

    //
    // 步骤8: 如果需要，分配MDL
    //
    if (Data->Iopb->Parameters.Write.MdlAddress != NULL) {
        newMdl = IoAllocateMdl(
            newBuffer,
            writeLength,
            FALSE,
            FALSE,
            NULL
        );

        if (newMdl == NULL) {
            TutorialDbgPrint("PreWrite: 分配MDL失败\n");
            SAFE_FREE(newBuffer, TUTORIAL_TAG);
            FltReleaseContext(fileContext);
            return FLT_PREOP_SUCCESS_NO_CALLBACK;
        }

        MmBuildMdlForNonPagedPool(newMdl);
    }

    //
    // 步骤9: 创建缓冲区交换上下文
    //
    swapContext = ExAllocatePoolWithTag(
        NonPagedPool,
        sizeof(TUTORIAL_BUFFER_SWAP_CONTEXT),
        TUTORIAL_TAG
    );

    if (swapContext == NULL) {
        TutorialDbgPrint("PreWrite: 分配交换上下文失败\n");
        SAFE_FREE_MDL(newMdl);
        SAFE_FREE(newBuffer, TUTORIAL_TAG);
        FltReleaseContext(fileContext);
        return FLT_PREOP_SUCCESS_NO_CALLBACK;
    }

    //
    // 步骤10: 保存原始信息
    //
    swapContext->OriginalBuffer = Data->Iopb->Parameters.Write.WriteBuffer;
    swapContext->OriginalMdl = Data->Iopb->Parameters.Write.MdlAddress;
    swapContext->SwappedBuffer = newBuffer;
    swapContext->SwappedMdl = newMdl;
    swapContext->BufferSize = writeLength;

    //
    // 步骤11: 替换IRP的缓冲区为加密后的缓冲区
    //
    Data->Iopb->Parameters.Write.WriteBuffer = newBuffer;
    Data->Iopb->Parameters.Write.MdlAddress = newMdl;

    FltSetCallbackDataDirty(Data);

    //
    // 步骤12: 如果是新文件，标记需要写入加密标识
    //
    if (!fileContext->IsEncrypted) {
        // 这是第一次写入，PostWrite需要添加加密标识
        fileContext->IsEncrypted = TRUE;
        TutorialDbgPrint("PreWrite: 新文件，需要添加加密标识\n");
    }

    //
    // 步骤13: 传递上下文给PostWrite
    //
    *CompletionContext = swapContext;

    FltReleaseContext(fileContext);

    TutorialDbgPrint("PreWrite: 缓冲区替换完成，密文将写入磁盘\n");

    return FLT_PREOP_SUCCESS_WITH_CALLBACK;
}

FLT_POSTOP_CALLBACK_STATUS
TutorialPostWrite(
    _Inout_ PFLT_CALLBACK_DATA Data,
    _In_ PCFLT_RELATED_OBJECTS FltObjects,
    _In_opt_ PVOID CompletionContext,
    _In_ FLT_POST_OPERATION_FLAGS Flags
)
/*++

功能说明:
    文件写入的Post回调
    
    主要任务：
    1. 如果是新加密的文件，在开头写入"TUTENC!!"标识
    2. 清理PreWrite分配的资源

参数:
    Data - I/O请求数据
    FltObjects - 过滤器相关对象
    CompletionContext - 从PreWrite传递的上下文
    Flags - 后处理标志

返回值:
    FLT_POSTOP_FINISHED_PROCESSING - 处理完成

--*/
{
    PTUTORIAL_BUFFER_SWAP_CONTEXT swapContext = NULL;
    PTUTORIAL_FILE_CONTEXT fileContext = NULL;
    NTSTATUS status;
    LARGE_INTEGER byteOffset;
    ULONG bytesWritten;
    UCHAR magic[TUTORIAL_MAGIC_SIZE];

    UNREFERENCED_PARAMETER(Flags);

    swapContext = (PTUTORIAL_BUFFER_SWAP_CONTEXT)CompletionContext;

    //
    // 步骤1: 检查写入是否成功
    //
    if (!NT_SUCCESS(Data->IoStatus.Status)) {
        TutorialDbgPrint("PostWrite: 写入失败, status = 0x%08X\n",
            Data->IoStatus.Status);
        goto Cleanup;
    }

    TutorialDbgPrint("PostWrite: 成功写入 %I64d 字节\n",
        Data->IoStatus.Information);

    //
    // 步骤2: 检查是否需要写入加密标识
    // 如果写入偏移为0（文件开头），需要添加魔术字节
    //
    if (Data->Iopb->Parameters.Write.ByteOffset.QuadPart == 0) {
        
        // 获取文件上下文
        status = FltGetStreamHandleContext(
            FltObjects->Instance,
            FltObjects->FileObject,
            &fileContext
        );

        if (NT_SUCCESS(status) && fileContext != NULL) {
            
            // 检查文件头是否已有加密标识
            byteOffset.QuadPart = 0;
            
            status = FltReadFile(
                FltObjects->Instance,
                FltObjects->FileObject,
                &byteOffset,
                TUTORIAL_MAGIC_SIZE,
                magic,
                FLTFL_IO_OPERATION_NON_CACHED,
                &bytesWritten,
                NULL,
                NULL
            );

            // 如果没有加密标识，写入"TUTENC!!"
            if (!NT_SUCCESS(status) ||
                RtlCompareMemory(magic, TUTORIAL_MAGIC, TUTORIAL_MAGIC_SIZE) != 
                TUTORIAL_MAGIC_SIZE) {
                
                TutorialDbgPrint("PostWrite: 写入加密标识\n");
                
                RtlCopyMemory(magic, TUTORIAL_MAGIC, TUTORIAL_MAGIC_SIZE);
                
                status = FltWriteFile(
                    FltObjects->Instance,
                    FltObjects->FileObject,
                    &byteOffset,
                    TUTORIAL_MAGIC_SIZE,
                    magic,
                    FLTFL_IO_OPERATION_NON_CACHED,
                    &bytesWritten,
                    NULL,
                    NULL
                );

                if (NT_SUCCESS(status)) {
                    TutorialDbgPrint("PostWrite: 加密标识写入成功\n");
                }
                else {
                    TutorialDbgPrint("PostWrite: 加密标识写入失败, status = 0x%08X\n",
                        status);
                }
            }

            FltReleaseContext(fileContext);
        }
    }

Cleanup:
    //
    // 步骤3: 清理资源
    //
    if (swapContext != NULL) {
        SAFE_FREE_MDL(swapContext->SwappedMdl);
        SAFE_FREE(swapContext->SwappedBuffer, TUTORIAL_TAG);
        SAFE_FREE(swapContext, TUTORIAL_TAG);
    }

    TutorialDbgPrint("PostWrite: 写入完成，资源已清理\n");

    return FLT_POSTOP_FINISHED_PROCESSING;
}
