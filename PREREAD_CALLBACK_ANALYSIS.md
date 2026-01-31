# Notepad打开TXT文件触发PreRead回调的实现机制分析

## 概述

本文档详细分析了基于Minifilter框架的透明加解密驱动（FOKS-TROT项目）是如何实现notepad.exe打开txt文件时触发PreRead回调的完整机制。

## 核心架构

该项目使用Windows内核的**Minifilter框架**实现文件系统过滤驱动，通过注册I/O回调函数来拦截和处理文件操作。

## 关键组件

### 1. 驱动注册 (DriverEntry)

**文件位置**: `Poc/Poc.c` (第48-600行)

驱动加载时的初始化流程：

```c
// 在DriverEntry函数中
status = FltRegisterFilter(DriverObject,
    &FilterRegistration,    // 过滤器注册结构
    &gFilterHandle);        // 返回的过滤器句柄

status = FltStartFiltering(gFilterHandle);  // 开始过滤I/O操作
```

**关键点**：
- `FltRegisterFilter`: 向过滤管理器注册minifilter驱动
- `FltStartFiltering`: 激活过滤器，开始拦截I/O请求
- 这两个API调用后，驱动就处于活动状态，开始监控文件系统操作

### 2. 回调注册表 (Callbacks Array)

**文件位置**: `Poc/Poc.c` (第328-359行)

```c
CONST FLT_OPERATION_REGISTRATION Callbacks[] = {
    // ... 其他操作 ...
    
    { IRP_MJ_CREATE,                    // 文件创建/打开操作
      0,
      PocPreCreateOperation,            // Pre回调
      PocPostCreateOperation },         // Post回调

    { IRP_MJ_READ,                      // 文件读取操作
      0,
      PocPreReadOperation,              // Pre读回调 ← 关键！
      PocPostReadOperation },           // Post读回调

    { IRP_MJ_WRITE,                     // 文件写入操作
      0,
      PocPreWriteOperation,
      PocPostWriteOperation },

    // ... 其他操作 ...
    
    { IRP_MJ_OPERATION_END }            // 结束标记
};
```

**关键点**：
- `IRP_MJ_READ`: I/O请求包(IRP)的主功能代码，表示读取操作
- `PocPreReadOperation`: 在读操作执行**之前**被调用的回调函数
- `PocPostReadOperation`: 在读操作执行**之后**被调用的回调函数
- 这个数组被注册到`FilterRegistration`结构中

### 3. 过滤器注册结构

**文件位置**: `Poc/Poc.c` (第382-402行)

```c
CONST FLT_REGISTRATION FilterRegistration = {
    sizeof(FLT_REGISTRATION),          // 结构大小
    FLT_REGISTRATION_VERSION,          // 版本
    0,                                 // 标志
    
    ContextRegistration,               // 上下文注册
    Callbacks,                         // 操作回调数组 ← 包含PreRead回调
    
    PocUnload,                         // 卸载回调
    PocInstanceSetup,                  // 实例设置
    NULL,                              // 实例查询卸载
    NULL,                              // 实例卸载开始
    NULL,                              // 实例卸载完成
    
    NULL,                              // 生成文件名
    NULL,                              // 生成目标文件名
    NULL                               // 规范化名称组件
};
```

## 完整调用流程

### 当用户使用notepad.exe打开txt文件时：

```
1. 用户操作: notepad.exe -> CreateFile("test.txt", GENERIC_READ, ...)
                    ↓
2. I/O管理器: 生成 IRP_MJ_CREATE 请求
                    ↓
3. 过滤管理器: 调用 PocPreCreateOperation (Poc.c:700)
                    ↓
4. PreCreate处理:
   - 获取文件名和扩展名
   - 检查是否为目标扩展名(txt, docx等)
   - 创建或查找StreamContext
   - 返回 FLT_PREOP_SUCCESS_WITH_CALLBACK
                    ↓
5. 文件系统: 执行实际的Create操作
                    ↓
6. 过滤管理器: 调用 PocPostCreateOperation (Poc.c:774)
   - PocPostCreateOperationWhenSafe (Poc.c:818)
   - 创建StreamContext，检查文件是否加密
   - 读取文件标识尾，判断 IsCipherText 状态
                    ↓
7. 用户操作: notepad.exe -> ReadFile(hFile, buffer, ...)
                    ↓
8. I/O管理器: 生成 IRP_MJ_READ 请求
                    ↓
9. 过滤管理器: 调用 PocPreReadOperation ← 这里！
                    ↓
10. PreRead处理: (详见下节)
                    ↓
11. 文件系统: 执行实际读取
                    ↓
12. 过滤管理器: 调用 PocPostReadOperation
                    ↓
13. PostRead处理: 解密数据(如果需要)
                    ↓
14. 返回数据给notepad.exe
```

## PocPreReadOperation详细分析

**文件位置**: `Poc/Read.c` (第25-400行)

### 函数签名

```c
FLT_PREOP_CALLBACK_STATUS
PocPreReadOperation(
    _Inout_ PFLT_CALLBACK_DATA Data,              // 回调数据
    _In_ PCFLT_RELATED_OBJECTS FltObjects,        // 相关对象
    _Flt_CompletionContext_Outptr_ PVOID* CompletionContext  // 完成上下文
)
```

### 核心处理逻辑

#### 1. 获取读取参数 (Read.c:54-58)

```c
StartingVbo = Data->Iopb->Parameters.Read.ByteOffset.QuadPart;  // 读取偏移
ByteCount = Data->Iopb->Parameters.Read.Length;                  // 读取长度
NonCachedIo = BooleanFlagOn(Data->Iopb->IrpFlags, IRP_NOCACHE);  // 是否非缓存I/O
```

#### 2. 查找StreamContext (Read.c:66-99)

```c
Status = PocFindOrCreateStreamContext(
    Data->Iopb->TargetInstance,
    Data->Iopb->TargetFileObject,
    FALSE,                              // 不创建，只查找
    &StreamContext,
    &ContextCreated);
```

**StreamContext包含**：
- `IsCipherText`: 文件是否已加密
- `FileSize`: 明文文件大小
- `FileName`: 文件名
- 其他加密相关信息

#### 3. 判断文件加密状态 (Read.c:101-110)

```c
if (!StreamContext->IsCipherText)
{
    // 未加密文件，不需要解密，直接放行
    Status = FLT_PREOP_SUCCESS_NO_CALLBACK;
    goto ERROR;
}
```

#### 4. 获取进程信息 (Read.c:113-177)

```c
Status = PocGetProcessName(Data, ProcessName);  // 获取进程名

eProcess = FltGetRequestorProcess(Data);        // 获取EPROCESS
ProcessId = PsGetProcessId(eProcess);           // 获取进程ID

// 查找进程是否在授权列表中
Status = PocFindProcessInfoNodeByPidEx(
    ProcessId,
    &OutProcessInfo,    // 输出进程信息
    FALSE,
    FALSE);
```

**进程权限类型**：
- **授权进程** (如notepad.exe): 读取明文数据，自动解密
- **非授权进程** (如wordpad.exe): 读取密文数据，不解密
- **备份进程** (如explorer.exe): 可读取完整密文文件（包括文件尾）

#### 5. 缓冲I/O处理 (Read.c:120-270)

对于缓冲读取（CachedIo），需要：

```c
if (!NonCachedIo)
{
    // 检查读取范围是否超出明文长度
    if (StartingVbo >= StreamContext->FileSize)
    {
        // 对于备份进程，允许读取文件尾
        if (OutProcessInfo && OutProcessInfo->OwnedProcessRule->Access == POC_PR_ACCESS_BACKUP)
        {
            // 允许读取
        }
        else
        {
            // 其他进程不允许读取超出明文大小的数据
            Data->IoStatus.Status = STATUS_END_OF_FILE;
            Status = FLT_PREOP_COMPLETE;
        }
    }
    else if (StartingVbo + ByteCount > StreamContext->FileSize)
    {
        // 读取长度超出明文大小，截断到明文大小
        Data->Iopb->Parameters.Read.Length = 
            (ULONG)(StreamContext->FileSize - StartingVbo);
    }
}
```

#### 6. 双缓冲机制 (Read.c:272-350)

为授权进程和非授权进程分配不同的缓冲区：

```c
// 分配新的缓冲区用于解密后的数据
NewBuffer = FltAllocatePoolAlignedWithTag(
    Data->Iopb->TargetInstance,
    NonPagedPool,
    (SIZE_T)ByteCount,
    POC_SWAP_BUFFER_TAG);

// 如果需要MDL
if (FlagOn(Data->Iopb->MinorFunction, IRP_MN_MDL))
{
    NewMdl = IoAllocateMdl(NewBuffer, (ULONG)ByteCount, FALSE, FALSE, NULL);
    MmBuildMdlForNonPagedPool(NewMdl);
}

// 创建SwapBuffer上下文
SwapBufferContext = ExAllocatePoolWithTag(
    NonPagedPool,
    sizeof(POC_SWAP_BUFFER_CONTEXT),
    POC_SWAP_BUFFER_TAG);

// 保存原始缓冲区信息
SwapBufferContext->OrigBuffer = Data->Iopb->Parameters.Read.ReadBuffer;
SwapBufferContext->OrigMdl = Data->Iopb->Parameters.Read.MdlAddress;

// 替换为新缓冲区
Data->Iopb->Parameters.Read.ReadBuffer = NewBuffer;
Data->Iopb->Parameters.Read.MdlAddress = NewMdl;

*CompletionContext = SwapBufferContext;  // 传递给PostRead
```

#### 7. 返回值决定后续处理

```c
return FLT_PREOP_SUCCESS_WITH_CALLBACK;  // 继续到PostRead进行解密
// 或
return FLT_PREOP_SUCCESS_NO_CALLBACK;    // 不需要PostRead处理
// 或
return FLT_PREOP_COMPLETE;               // 已完成，不继续向下传递
```

## 关键技术点

### 1. Minifilter框架机制

- **层次化过滤**: minifilter驱动位于文件系统驱动之上，I/O管理器之下
- **Pre/Post回调**: 可以在I/O操作前后进行处理
- **上下文管理**: 通过StreamContext跟踪文件状态

### 2. 双缓冲技术

```
授权进程视图:
┌─────────────┐
│  明文缓冲区  │ ← 解密后的数据
└─────────────┘

非授权进程视图:
┌─────────────┐
│  密文缓冲区  │ ← 原始加密数据
└─────────────┘
```

**实现原理**：
- PreRead时分配新缓冲区
- 文件系统读取密文数据到新缓冲区
- PostRead时根据进程权限决定是否解密
- 授权进程：解密后复制到应用程序缓冲区
- 非授权进程：直接复制密文数据

### 3. 文件大小管理

```
文件物理布局:
|<------- 明文数据 ------->|<-- 扩展区 -->|<--- 文件标识尾 (4KB) --->|
0                    FileSize          SectorAlign              Allocation

StreamContext->FileSize: 明文文件大小（应用层看到的大小）
Fcb->FileSize: 物理文件大小（包括标识尾）
```

### 4. 进程权限控制

在`Config.c`中配置：

```c
// 授权进程列表（可读取明文）
WCHAR* PocAccessGrandProcessRule[] = {
    L"notepad.exe",      // ← notepad是授权进程
    L"WINWORD.EXE",
    L"EXCEL.EXE",
    // ...
};

// 非授权进程（只能读取密文）
// 所有不在授权列表中的进程

// 备份权限进程（可读取完整密文）
WCHAR* PocAccessBackupProcessRule[] = {
    L"explorer.exe",
    L"vmtoolsd.exe",
    // ...
};
```

## 触发条件总结

Notepad打开txt文件时触发PreRead回调需要满足：

1. ✅ **驱动已加载**: `FltStartFiltering`已调用
2. ✅ **目标扩展名**: 文件扩展名在监控列表中(txt, docx等)
3. ✅ **文件已加密**: `StreamContext->IsCipherText == TRUE`
4. ✅ **读取操作**: 应用程序调用`ReadFile`等读取API
5. ✅ **I/O管理器生成IRP**: 系统生成`IRP_MJ_READ`请求
6. ✅ **过滤管理器分发**: 根据注册的Callbacks调用`PocPreReadOperation`

## 调试验证

可以通过以下方式验证PreRead回调被触发：

### 1. DebugView日志

```
找到注册表项：
HKEY_LOCAL_MACHINE\SYSTEM\CurrentControlSet\Control\Session Manager\Debug Print Filter
新建DWORD值 "default" = 0xF

重启后运行DebugView (管理员权限)
启用 Capture->Capture Kernel

在代码中PT_DBG_PRINT输出的日志会显示在DebugView中
```

### 2. 代码中已有的调试输出

在`Read.c`的PreRead函数中有多处日志输出，例如：

```c
PT_DBG_PRINT(PTDBG_TRACE_ROUTINES, 
    ("%s->PocFindOrCreateStreamContext failed. Status = 0x%x.\n",
    __FUNCTION__, Status));

PT_DBG_PRINT(PTDBG_TRACE_ROUTINES,
    ("%s->%ws cachedio read end of file %ws Length = %d.\n",
    __FUNCTION__, ProcessName, StreamContext->FileName, ...));
```

## 相关文件清单

| 文件路径 | 说明 | 关键内容 |
|---------|------|---------|
| `Poc/Poc.c` | 驱动主文件 | 驱动注册、回调注册表、Create操作 |
| `Poc/Read.c` | 读操作处理 | PreRead/PostRead实现、解密逻辑 |
| `Poc/read.h` | 读操作头文件 | PreRead/PostRead函数声明 |
| `Poc/Write.c` | 写操作处理 | PreWrite/PostWrite实现、加密逻辑 |
| `Poc/Context.c` | 上下文管理 | StreamContext创建和管理 |
| `Poc/Config.c` | 配置文件 | 授权进程列表、扩展名列表 |
| `Poc/Process.c` | 进程管理 | 进程权限判断 |
| `Poc/cipher.c` | 加解密实现 | AES加解密算法 |
| `Poc/Utils.c` | 工具函数 | 辅助函数 |

## 参考资料

- Windows Driver Kit (WDK) - Minifilter框架文档
- `README.md` - 项目总体介绍和使用说明
- `TESTMANUAL.md` - 测试手册
- Windows内核源码 - IRP处理机制
- 项目中的论文和开发文档（如有）

## 总结

Notepad打开txt文件触发PreRead回调的核心机制是：

1. **Minifilter框架注册**: 通过`FltRegisterFilter`注册过滤器，将`PocPreReadOperation`与`IRP_MJ_READ`关联
2. **I/O拦截**: 当任何进程（包括notepad.exe）对txt文件执行读取操作时，I/O管理器生成`IRP_MJ_READ`请求
3. **过滤管理器分发**: Filter Manager根据注册的回调表，在读操作执行前调用`PocPreReadOperation`
4. **PreRead处理**: 在PreRead中判断文件加密状态、进程权限，决定是否需要解密
5. **双缓冲机制**: 为授权进程准备解密缓冲区，为非授权进程保持密文
6. **PostRead解密**: 在PostRead中根据进程权限执行实际的解密操作

整个过程是**自动的、透明的**，应用程序（notepad.exe）无需任何修改，就能读取解密后的明文数据，而非授权进程读到的是加密后的密文数据。这就是透明加解密的核心原理。
