# PreRead回调触发流程图

## 系统架构层次

```
┌─────────────────────────────────────────────────────────┐
│              用户态 (User Mode)                           │
│                                                          │
│  ┌──────────────┐        ┌──────────────┐               │
│  │ notepad.exe  │        │ wordpad.exe  │               │
│  │  (授权进程)   │        │  (非授权进程) │               │
│  └──────┬───────┘        └──────┬───────┘               │
│         │ CreateFile/ReadFile   │                        │
│         │                       │                        │
└─────────┼───────────────────────┼────────────────────────┘
          │                       │
          ▼                       ▼
┌─────────────────────────────────────────────────────────┐
│              内核态 (Kernel Mode)                         │
│                                                          │
│  ┌───────────────────────────────────────────────────┐  │
│  │         I/O 管理器 (I/O Manager)                   │  │
│  │   生成 IRP (I/O Request Packet)                   │  │
│  │   - IRP_MJ_CREATE (文件打开)                       │  │
│  │   - IRP_MJ_READ   (文件读取)                       │  │
│  └─────────────────┬─────────────────────────────────┘  │
│                    │                                     │
│                    ▼                                     │
│  ┌───────────────────────────────────────────────────┐  │
│  │   过滤管理器 (Filter Manager)                      │  │
│  │   分发IRP到注册的Minifilter驱动                    │  │
│  └─────────────────┬─────────────────────────────────┘  │
│                    │                                     │
│                    ▼                                     │
│  ┌───────────────────────────────────────────────────┐  │
│  │   Poc Minifilter 驱动                             │  │
│  │   ┌─────────────────────────────────────────┐     │  │
│  │   │ FilterRegistration 结构                  │     │  │
│  │   │  - Callbacks[] 回调数组                 │     │  │
│  │   │    ├─ IRP_MJ_CREATE                     │     │  │
│  │   │    │   ├─ PocPreCreateOperation         │     │  │
│  │   │    │   └─ PocPostCreateOperation        │     │  │
│  │   │    ├─ IRP_MJ_READ ◄────────────────────┐│     │  │
│  │   │    │   ├─ PocPreReadOperation    ◄─── 关键!   │  │
│  │   │    │   └─ PocPostReadOperation         ││     │  │
│  │   │    ├─ IRP_MJ_WRITE                     ││     │  │
│  │   │    └─ ...                               ││     │  │
│  │   └─────────────────────────────────────────┘│     │  │
│  └───────────────────────────────────────────────┘     │
│                    │                                     │
│                    ▼                                     │
│  ┌───────────────────────────────────────────────────┐  │
│  │   文件系统驱动 (NTFS/FAT32 等)                     │  │
│  │   执行实际的磁盘I/O操作                            │  │
│  └─────────────────┬─────────────────────────────────┘  │
│                    │                                     │
└────────────────────┼─────────────────────────────────────┘
                     │
                     ▼
          ┌──────────────────┐
          │   物理磁盘         │
          │   密文数据存储     │
          └──────────────────┘
```

## Notepad打开TXT文件的详细流程

```
用户操作
   │
   ├─> 1. 双击 test.txt 文件
   │
   ▼
notepad.exe 启动
   │
   ├─> 2. 调用 CreateFile("C:\Desktop\test.txt", GENERIC_READ, ...)
   │
   ▼
内核: I/O管理器
   │
   ├─> 3. 生成 IRP_MJ_CREATE
   │
   ▼
Filter Manager
   │
   ├─> 4. 调用 PocPreCreateOperation()
   │      ├─ 获取文件名: test.txt
   │      ├─ 检查扩展名: .txt ✓ (在允许列表中)
   │      ├─ 检查路径: C:\Desktop ✓ (在监控路径中)
   │      └─ 返回: FLT_PREOP_SUCCESS_WITH_CALLBACK
   │
   ▼
NTFS驱动
   │
   ├─> 5. 执行实际的Create操作
   │      └─ 打开文件句柄
   │
   ▼
Filter Manager
   │
   ├─> 6. 调用 PocPostCreateOperation()
   │      └─ PocPostCreateOperationWhenSafe()
   │         ├─ 创建/查找 StreamContext
   │         ├─ 读取文件最后4KB (文件标识尾)
   │         ├─ 检查加密标识: "POCENC" ✓
   │         ├─ 设置 StreamContext->IsCipherText = TRUE
   │         ├─ 保存 StreamContext->FileSize = 明文大小
   │         └─ 返回文件句柄给应用程序
   │
   ▼
notepad.exe
   │
   ├─> 7. 调用 ReadFile(hFile, buffer, 4096, ...)
   │
   ▼
内核: I/O管理器
   │
   ├─> 8. 生成 IRP_MJ_READ
   │      ├─ ByteOffset: 0
   │      ├─ Length: 4096
   │      └─ Buffer: 0x12345678 (notepad的缓冲区)
   │
   ▼
Filter Manager
   │
   ├─> 9. 调用 PocPreReadOperation() ◄──────── 这里触发PreRead!
   │      │
   │      ├─ Step 1: 获取读取参数
   │      │   ├─ StartingVbo = 0
   │      │   ├─ ByteCount = 4096
   │      │   └─ NonCachedIo = FALSE (缓冲I/O)
   │      │
   │      ├─ Step 2: 查找StreamContext
   │      │   └─ StreamContext->IsCipherText = TRUE ✓
   │      │
   │      ├─ Step 3: 获取进程信息
   │      │   ├─ ProcessName = "notepad.exe"
   │      │   ├─ 查找授权列表
   │      │   └─ OutProcessInfo->Access = POC_PR_ACCESS_GRAND ✓
   │      │
   │      ├─ Step 4: 检查读取范围
   │      │   ├─ StartingVbo (0) < FileSize (1024) ✓
   │      │   └─ 范围合法
   │      │
   │      ├─ Step 5: 分配解密缓冲区 (双缓冲机制)
   │      │   ├─ NewBuffer = ExAllocatePool(4096字节)
   │      │   ├─ SwapBufferContext->OrigBuffer = 0x12345678
   │      │   ├─ SwapBufferContext->NewBuffer = 0xABCDEF00
   │      │   └─ 替换IRP的ReadBuffer = 0xABCDEF00
   │      │
   │      └─ Step 6: 返回
   │          ├─ *CompletionContext = SwapBufferContext
   │          └─ Return: FLT_PREOP_SUCCESS_WITH_CALLBACK
   │
   ▼
NTFS驱动
   │
   ├─> 10. 从磁盘读取密文数据
   │       └─ 读到 NewBuffer (0xABCDEF00)
   │
   ▼
Filter Manager
   │
   ├─> 11. 调用 PocPostReadOperation()
   │       │
   │       ├─ Step 1: 检查进程权限
   │       │   └─ notepad.exe 是授权进程 ✓
   │       │
   │       ├─ Step 2: 解密数据
   │       │   ├─ 从 NewBuffer 读取密文
   │       │   ├─ AES-128 ECB 解密
   │       │   └─ 解密结果写入 NewBuffer
   │       │
   │       ├─ Step 3: 复制到原始缓冲区
   │       │   └─ memcpy(OrigBuffer, NewBuffer, 4096)
   │       │       从 0xABCDEF00 → 0x12345678
   │       │
   │       └─ Step 4: 清理
   │           ├─ ExFreePool(NewBuffer)
   │           ├─ ExFreePool(SwapBufferContext)
   │           └─ 返回: FLT_POSTOP_FINISHED_PROCESSING
   │
   ▼
I/O管理器
   │
   ├─> 12. 完成IRP
   │       └─ IoStatus.Information = 4096 (已读字节数)
   │
   ▼
notepad.exe
   │
   └─> 13. ReadFile 返回
          ├─ buffer (0x12345678) 包含解密后的明文
          └─ notepad显示明文内容给用户


═══════════════════════════════════════════════════════════

对比: WordPad (非授权进程) 打开同一文件

wordpad.exe
   │
   ├─> ReadFile(hFile, buffer, 4096, ...)
   │
   ▼
PocPreReadOperation()
   │
   ├─ ProcessName = "wordpad.exe"
   ├─ 查找授权列表: 未找到 ✗
   ├─ OutProcessInfo = NULL
   ├─ 使用密文缓冲区 (不分配新缓冲区)
   └─ Return: FLT_PREOP_SUCCESS_NO_CALLBACK
   │
   ▼
PocPostReadOperation() - 不被调用!
   │
   ▼
wordpad.exe
   │
   └─> buffer 包含加密后的密文 (乱码)
       └─ wordpad显示乱码给用户
```

## StreamContext 数据结构

```c
typedef struct _POC_STREAM_CONTEXT {
    // 文件基本信息
    WCHAR FileName[POC_MAX_NAME_LENGTH];      // 文件名
    WCHAR FileExtension[POC_MAX_NAME_LENGTH]; // 扩展名
    
    // 加密状态
    BOOLEAN IsCipherText;     // 是否已加密 ← 关键字段!
    BOOLEAN IsDirty;          // 是否有未保存的修改
    
    // 文件大小管理
    LONGLONG FileSize;        // 明文文件大小 (应用层看到的)
    LONGLONG RealFileSize;    // 实际文件大小 (包括标识尾)
    
    // 双缓冲相关
    PVOID PlainBuffer;        // 明文缓冲区指针
    PVOID CipherBuffer;       // 密文缓冲区指针
    
    // 其他字段...
} POC_STREAM_CONTEXT, *PPOC_STREAM_CONTEXT;
```

## 文件物理布局

```
未加密文件 (IsCipherText = FALSE):
┌──────────────────────────────────────┐
│     原始明文数据                       │
│     "Hello World"                     │
└──────────────────────────────────────┘
 0                                  FileSize


已加密文件 (IsCipherText = TRUE):
┌─────────────┬──────────────┬────────────────────┐
│  加密数据    │   扩展区     │   文件标识尾 (4KB)  │
│  (密文)      │  (对齐填充)  │   "POCENC..."       │
└─────────────┴──────────────┴────────────────────┘
 0        FileSize      SectorAlign          RealFileSize
           │                                      │
           │                                      │
  授权进程读到这里                      备份进程读到这里
  (明文长度)                           (完整密文)
```

## 双缓冲机制详解

```
1. 授权进程 (notepad.exe) 读取流程:

   应用程序缓冲区          驱动分配的新缓冲区         磁盘
   ┌──────────────┐       ┌──────────────┐       ┌──────────┐
   │              │       │              │       │          │
   │  等待明文... │ ◄───  │   密文数据   │ ◄───  │  密文    │
   │              │  解密 │              │  读取 │          │
   └──────────────┘       └──────────────┘       └──────────┘
   OrigBuffer             NewBuffer              Physical File
   0x12345678             0xABCDEF00

   PreRead:  替换IRP缓冲区指向NewBuffer
   PostRead: 解密NewBuffer，复制到OrigBuffer


2. 非授权进程 (wordpad.exe) 读取流程:

   应用程序缓冲区                                  磁盘
   ┌──────────────┐                           ┌──────────┐
   │              │                           │          │
   │   密文数据   │ ◄──────────────────────── │  密文    │
   │   (乱码)     │        直接读取            │          │
   └──────────────┘                           └──────────┘
   OrigBuffer                                 Physical File
   0x12345678

   PreRead:  不替换缓冲区，返回NO_CALLBACK
   PostRead: 不被调用，数据直接返回给应用程序
```

## 关键判断逻辑

```
PocPreReadOperation() 返回值决定树:

                    开始
                     │
                     ▼
              文件已加密?
             (IsCipherText)
                /     \
              否       是
              /         \
             ▼           ▼
    返回NO_CALLBACK  获取进程信息
    (不需要处理)         │
                         ▼
                    授权进程?
                    /        \
                  是          否
                  /            \
                 ▼              ▼
         分配新缓冲区      返回NO_CALLBACK
         双缓冲机制       (读取密文)
              │
              ▼
    返回SUCCESS_WITH_CALLBACK
         (需要PostRead解密)
```

## 性能优化点

1. **缓冲I/O检测**: 只对缓冲I/O进行进程权限判断
2. **文件大小截断**: PreRead时就限制读取范围，避免读取无用数据
3. **上下文缓存**: StreamContext缓存文件加密状态，避免重复检查文件标识尾
4. **非目标文件快速放行**: 不符合扩展名/路径的文件立即返回NO_CALLBACK
5. **内存池管理**: 使用NonPagedPool保证内核态性能

## 总结

整个PreRead回调触发的关键在于：

1. ✅ **Minifilter框架自动分发**: Filter Manager根据IRP类型自动调用注册的回调
2. ✅ **透明拦截**: 应用程序无感知，所有进程的读取操作都会被拦截
3. ✅ **智能处理**: 根据文件加密状态和进程权限决定是否需要解密
4. ✅ **双缓冲隔离**: 授权和非授权进程读取到不同的数据视图
5. ✅ **安全保护**: 密文数据在磁盘上始终保持加密状态

这就是Windows内核态透明加解密的核心实现机制！
