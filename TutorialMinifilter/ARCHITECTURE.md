# 教学版透明加解密驱动 - 架构图解

## 系统架构层次

```
┌─────────────────────────────────────────────────────────────────┐
│                         用户态 (User Mode)                       │
├─────────────────────────────────────────────────────────────────┤
│                                                                 │
│  ┌────────────────┐              ┌────────────────┐            │
│  │  notepad.exe   │              │  wordpad.exe   │            │
│  │   (授权进程)    │              │  (非授权进程)   │            │
│  └───────┬────────┘              └───────┬────────┘            │
│          │                               │                      │
│          │ CreateFile/ReadFile/WriteFile │                      │
│          │                               │                      │
└──────────┼───────────────────────────────┼──────────────────────┘
           │                               │
           ▼                               ▼
┌─────────────────────────────────────────────────────────────────┐
│                         内核态 (Kernel Mode)                     │
├─────────────────────────────────────────────────────────────────┤
│                                                                 │
│  ┌─────────────────────────────────────────────────────────┐   │
│  │             I/O 管理器 (I/O Manager)                     │   │
│  │   生成 IRP (I/O Request Packet)                         │   │
│  │   - IRP_MJ_CREATE  (CreateFile)                        │   │
│  │   - IRP_MJ_READ    (ReadFile)                          │   │
│  │   - IRP_MJ_WRITE   (WriteFile)                         │   │
│  └──────────────────────┬──────────────────────────────────┘   │
│                         │                                       │
│                         ▼                                       │
│  ┌─────────────────────────────────────────────────────────┐   │
│  │       过滤管理器 (Filter Manager)                        │   │
│  │   分发IRP到注册的Minifilter驱动                          │   │
│  └──────────────────────┬──────────────────────────────────┘   │
│                         │                                       │
│                         ▼                                       │
│  ┌─────────────────────────────────────────────────────────┐   │
│  │         TutorialMinifilter 驱动                          │   │
│  │                                                         │   │
│  │  ┌──────────────────────────────────────────────────┐  │   │
│  │  │  TutorialDriver.c                                │  │   │
│  │  │  ┌────────────────────────────────────────────┐  │  │   │
│  │  │  │ FilterRegistration                         │  │  │   │
│  │  │  │  - Callbacks[] 回调函数数组                 │  │  │   │
│  │  │  │    ├─ IRP_MJ_CREATE                        │  │  │   │
│  │  │  │    │   ├─ TutorialPreCreate  ◄─┐           │  │  │   │
│  │  │  │    │   └─ TutorialPostCreate   │           │  │  │   │
│  │  │  │    ├─ IRP_MJ_READ              │           │  │  │   │
│  │  │  │    │   ├─ TutorialPreRead  ◄───┼─ 核心！   │  │  │   │
│  │  │  │    │   └─ TutorialPostRead     │           │  │  │   │
│  │  │  │    └─ IRP_MJ_WRITE             │           │  │  │   │
│  │  │  │        ├─ TutorialPreWrite  ◄──┘           │  │  │   │
│  │  │  │        └─ TutorialPostWrite                │  │  │   │
│  │  │  └────────────────────────────────────────────┘  │  │   │
│  │  └──────────────────────────────────────────────────┘  │   │
│  │                                                         │   │
│  │  辅助模块:                                               │   │
│  │  - TutorialConfig.c   (进程/文件检查)                    │   │
│  │  - TutorialCipher.c   (XOR加密)                         │   │
│  └─────────────────────────────────────────────────────────┘   │
│                         │                                       │
│                         ▼                                       │
│  ┌─────────────────────────────────────────────────────────┐   │
│  │         文件系统驱动 (NTFS Driver)                       │   │
│  │   执行实际的磁盘I/O操作                                  │   │
│  └──────────────────────┬──────────────────────────────────┘   │
│                         │                                       │
└─────────────────────────┼───────────────────────────────────────┘
                          │
                          ▼
                  ┌────────────────┐
                  │   物理磁盘      │
                  │  密文存储       │
                  └────────────────┘
```

## Notepad写入TXT文件 - 详细流程

```
用户操作: notepad.exe 输入 "Hello World" 并保存
   │
   ▼
┌─────────────────────────────────────────────────────────┐
│  1. CreateFile("test.txt", GENERIC_WRITE, ...)         │
├─────────────────────────────────────────────────────────┤
   │
   ├─> IRP_MJ_CREATE 请求
   │
   ├─> TutorialPreCreate()
   │   ├─ 获取文件名: "test.txt"
   │   ├─ 检查扩展名: .txt ✓
   │   └─ 返回: CALLBACK_NEEDED
   │
   ├─> NTFS驱动执行Create
   │
   ├─> TutorialPostCreate()
   │   ├─ 创建文件上下文 (FileContext)
   │   ├─ 读取文件头 → 无"TUTENC!!"
   │   ├─ 设置: IsEncrypted = FALSE
   │   └─ 返回句柄给notepad
   │
   ▼
┌─────────────────────────────────────────────────────────┐
│  2. WriteFile(hFile, "Hello World", 11, ...)           │
├─────────────────────────────────────────────────────────┤
   │
   ├─> IRP_MJ_WRITE 请求
   │   ├─ Buffer: "Hello World"
   │   ├─ Length: 11
   │   └─ Offset: 0
   │
   ├─> TutorialPreWrite()
   │   │
   │   ├─ 检查进程: notepad.exe ✓
   │   ├─ 分配新缓冲区 (11字节)
   │   │
   │   ├─ 复制明文: "Hello World"
   │   │   原始: 48 65 6C 6C 6F 20 57 6F 72 6C 64
   │   │
   │   ├─ XOR加密 (密钥=0x42):
   │   │   密文: 0A 27 2E 2E 2D 62 15 2D 30 2E 06
   │   │
   │   ├─ 替换IRP缓冲区:
   │   │   原始: "Hello World"
   │   │   新: [密文数据]
   │   │
   │   └─ 返回: CALLBACK_NEEDED
   │
   ├─> NTFS驱动写入磁盘:
   │   └─ 写入密文: 0A 27 2E 2E 2D 62 15 2D 30 2E 06
   │
   ├─> TutorialPostWrite()
   │   ├─ 检查写入偏移 = 0 ✓
   │   ├─ 写入魔术字节: "TUTENC!!" (54 55 54 45 4E 43 21 21)
   │   └─ 设置: IsEncrypted = TRUE
   │
   ├─> 清理缓冲区和上下文
   │
   ▼
┌─────────────────────────────────────────────────────────┐
│  3. CloseHandle(hFile)                                 │
├─────────────────────────────────────────────────────────┤
   │
   └─> 文件关闭，上下文释放
   
磁盘上的文件内容:
┌──────────────────────────────────────────────────┐
│  [0-7]:   54 55 54 45 4E 43 21 21  "TUTENC!!"    │
│  [8-18]:  0A 27 2E 2E 2D 62 15 2D 30 2E 06       │
│           (加密后的"Hello World")                 │
└──────────────────────────────────────────────────┘
```

## Notepad读取TXT文件 - 详细流程

```
用户操作: notepad.exe 打开 test.txt
   │
   ▼
┌─────────────────────────────────────────────────────────┐
│  1. CreateFile("test.txt", GENERIC_READ, ...)          │
├─────────────────────────────────────────────────────────┤
   │
   ├─> IRP_MJ_CREATE 请求
   │
   ├─> TutorialPreCreate()
   │   └─ 检查: .txt ✓
   │
   ├─> NTFS驱动执行Create
   │
   ├─> TutorialPostCreate()
   │   ├─ 读取文件头8字节
   │   ├─ 检测到: "TUTENC!!" ✓
   │   ├─ 设置: IsEncrypted = TRUE
   │   └─ 返回句柄
   │
   ▼
┌─────────────────────────────────────────────────────────┐
│  2. ReadFile(hFile, buffer, 1024, ...)                 │
├─────────────────────────────────────────────────────────┤
   │
   ├─> IRP_MJ_READ 请求
   │   ├─ OrigBuffer: 0x12345678 (notepad的缓冲区)
   │   ├─ Length: 1024
   │   └─ Offset: 0
   │
   ├─> TutorialPreRead()
   │   │
   │   ├─ 检查文件: IsEncrypted = TRUE ✓
   │   ├─ 检查进程: notepad.exe ✓
   │   │
   │   ├─ 分配新缓冲区:
   │   │   NewBuffer: 0xABCDEF00 (1024字节)
   │   │
   │   ├─ 保存交换上下文:
   │   │   SwapContext {
   │   │     OriginalBuffer: 0x12345678
   │   │     SwappedBuffer:  0xABCDEF00
   │   │     BufferSize:     1024
   │   │   }
   │   │
   │   ├─ 替换IRP缓冲区:
   │   │   原始: 0x12345678 (notepad的)
   │   │   新:   0xABCDEF00 (驱动的)
   │   │
   │   └─ 返回: CALLBACK_NEEDED
   │
   ├─> NTFS驱动读取磁盘:
   │   └─> 读取到 NewBuffer (0xABCDEF00)
   │       [0-7]:  54 55 54 45 4E 43 21 21 "TUTENC!!"
   │       [8-18]: 0A 27 2E 2E 2D 62 15 2D 30 2E 06 (密文)
   │
   ├─> TutorialPostRead()
   │   │
   │   ├─ 从 NewBuffer 获取数据
   │   │
   │   ├─ 跳过魔术字节 (偏移=0)
   │   │
   │   ├─ XOR解密 (密钥=0x42):
   │   │   密文: 0A 27 2E 2E 2D 62 15 2D 30 2E 06
   │   │   明文: 48 65 6C 6C 6F 20 57 6F 72 6C 64
   │   │         "H  e  l  l  o     W  o  r  l  d"
   │   │
   │   ├─ 复制明文到原始缓冲区:
   │   │   从: NewBuffer  (0xABCDEF00)
   │   │   到: OrigBuffer (0x12345678)
   │   │   内容: "TUTENC!!Hello World"
   │   │
   │   ├─ 清理资源:
   │   │   └─> 释放 NewBuffer (0xABCDEF00)
   │   │
   │   └─ 返回
   │
   ▼
┌─────────────────────────────────────────────────────────┐
│  3. notepad.exe 从buffer读到明文                        │
│     显示: "Hello World"                                 │
└─────────────────────────────────────────────────────────┘
```

## WordPad读取同一文件 - 对比流程

```
用户操作: wordpad.exe 打开同一个 test.txt
   │
   ▼
┌─────────────────────────────────────────────────────────┐
│  1. CreateFile - 同notepad                             │
├─────────────────────────────────────────────────────────┤
   │
   └─> TutorialPostCreate: IsEncrypted = TRUE
   
┌─────────────────────────────────────────────────────────┐
│  2. ReadFile                                           │
├─────────────────────────────────────────────────────────┤
   │
   ├─> IRP_MJ_READ 请求
   │
   ├─> TutorialPreRead()
   │   ├─ 检查文件: IsEncrypted = TRUE ✓
   │   ├─ 检查进程: wordpad.exe ✗
   │   │
   │   └─ 返回: NO_CALLBACK (不解密！)
   │
   ├─> NTFS驱动直接读取:
   │   └─> 读取到 wordpad的缓冲区
   │       [0-7]:  54 55 54 45 4E 43 21 21 "TUTENC!!"
   │       [8-18]: 0A 27 2E 2E 2D 62 15 2D 30 2E 06 (密文)
   │
   └─> TutorialPostRead() - 不会被调用
   
┌─────────────────────────────────────────────────────────┐
│  3. wordpad.exe 直接看到密文                            │
│     显示: "TUTENC!!" + 乱码                             │
└─────────────────────────────────────────────────────────┘
```

## 核心数据结构

### 1. 文件上下文 (TUTORIAL_FILE_CONTEXT)

```c
typedef struct _TUTORIAL_FILE_CONTEXT {
    BOOLEAN IsEncrypted;     // 文件是否已加密
    BOOLEAN IsMonitored;     // 文件是否在监控范围内
} TUTORIAL_FILE_CONTEXT;

生命周期:
- Create时创建
- Close时自动释放
- 用FltSetStreamHandleContext设置
- 用FltGetStreamHandleContext获取
```

### 2. 缓冲区交换上下文 (TUTORIAL_BUFFER_SWAP_CONTEXT)

```c
typedef struct _TUTORIAL_BUFFER_SWAP_CONTEXT {
    PVOID OriginalBuffer;    // 原始缓冲区（应用程序的）
    PVOID SwappedBuffer;     // 交换的缓冲区（驱动分配的）
    PMDL OriginalMdl;        // 原始MDL
    PMDL SwappedMdl;         // 交换的MDL
    ULONG BufferSize;        // 缓冲区大小
} TUTORIAL_BUFFER_SWAP_CONTEXT;

用途:
- Pre回调中创建
- 通过CompletionContext传递给Post回调
- Post回调中使用后释放
```

## 缓冲区交换详解

### 读取场景（Read）

```
Pre回调时:
┌─────────────────────────────────────────────────────┐
│  应用程序缓冲区 (OriginalBuffer)                     │
│  地址: 0x12345678                                   │
│  内容: [空，等待数据]                                │
└─────────────────────────────────────────────────────┘
         ▲
         │ 保存地址
         │
┌────────┴────────────────────────────────────────────┐
│  交换上下文                                          │
│    OriginalBuffer = 0x12345678                     │
│    SwappedBuffer  = 0xABCDEF00                     │
└─────────────────────────────────────────────────────┘
         │
         │ 分配新缓冲区
         ▼
┌─────────────────────────────────────────────────────┐
│  驱动分配缓冲区 (SwappedBuffer)                      │
│  地址: 0xABCDEF00                                   │
│  内容: [空，准备接收密文]                            │
└─────────────────────────────────────────────────────┘
         │
         │ 替换IRP->ReadBuffer
         ▼
     文件系统读取密文到这里

Post回调时:
┌─────────────────────────────────────────────────────┐
│  驱动分配缓冲区 (SwappedBuffer)                      │
│  地址: 0xABCDEF00                                   │
│  内容: [密文数据]                                    │
└─────────────────────────────────────────────────────┘
         │
         │ XOR解密
         ▼
      [明文数据]
         │
         │ RtlCopyMemory
         ▼
┌─────────────────────────────────────────────────────┐
│  应用程序缓冲区 (OriginalBuffer)                     │
│  地址: 0x12345678                                   │
│  内容: [明文数据] ← 应用程序读到这里                 │
└─────────────────────────────────────────────────────┘
```

### 写入场景（Write）

```
Pre回调时:
┌─────────────────────────────────────────────────────┐
│  应用程序缓冲区 (OriginalBuffer)                     │
│  地址: 0x12345678                                   │
│  内容: [明文数据]                                    │
└─────────────────────────────────────────────────────┘
         │
         │ 复制到新缓冲区
         ▼
┌─────────────────────────────────────────────────────┐
│  驱动分配缓冲区 (SwappedBuffer)                      │
│  地址: 0xABCDEF00                                   │
│  内容: [明文数据] → XOR加密 → [密文数据]             │
└─────────────────────────────────────────────────────┘
         │
         │ 替换IRP->WriteBuffer
         ▼
     文件系统写入密文到磁盘
```

## 关键API调用链

### 驱动初始化

```
DriverEntry()
  └─> FltRegisterFilter(DriverObject, &FilterRegistration, &gFilterHandle)
       └─> 注册回调函数表
  └─> FltStartFiltering(gFilterHandle)
       └─> 激活过滤器，开始拦截I/O
```

### 文件打开

```
应用: CreateFile()
  └─> 系统: IRP_MJ_CREATE
       └─> Filter Manager分发
            ├─> TutorialPreCreate()
            │    └─> FltGetFileNameInformation() - 获取文件名
            │    └─> FltParseFileNameInformation() - 解析文件名
            │    └─> TutorialIsMonitoredFile() - 检查是否监控
            │
            ├─> NTFS执行Create
            │
            └─> TutorialPostCreate()
                 ├─> FltAllocateContext() - 分配文件上下文
                 ├─> FltReadFile() - 读取文件头
                 ├─> 检查"TUTENC!!"标识
                 └─> FltSetStreamHandleContext() - 设置上下文
```

### 文件读取（授权进程）

```
应用: ReadFile()
  └─> 系统: IRP_MJ_READ
       └─> Filter Manager分发
            ├─> TutorialPreRead()
            │    ├─> FltGetStreamHandleContext() - 获取文件上下文
            │    ├─> TutorialIsAuthorizedProcess() - 检查进程
            │    ├─> ExAllocatePoolWithTag() - 分配新缓冲区
            │    ├─> 替换IRP->ReadBuffer
            │    └─> 返回CALLBACK_NEEDED
            │
            ├─> NTFS读取密文到新缓冲区
            │
            └─> TutorialPostRead()
                 ├─> TutorialXorEncryptDecrypt() - 解密
                 ├─> RtlCopyMemory() - 复制明文到原始缓冲区
                 └─> ExFreePoolWithTag() - 释放新缓冲区
```

### 文件写入（授权进程）

```
应用: WriteFile()
  └─> 系统: IRP_MJ_WRITE
       └─> Filter Manager分发
            ├─> TutorialPreWrite()
            │    ├─> FltGetStreamHandleContext() - 获取文件上下文
            │    ├─> TutorialIsAuthorizedProcess() - 检查进程
            │    ├─> ExAllocatePoolWithTag() - 分配新缓冲区
            │    ├─> RtlCopyMemory() - 复制明文
            │    ├─> TutorialXorEncryptDecrypt() - 加密
            │    ├─> 替换IRP->WriteBuffer
            │    └─> 返回CALLBACK_NEEDED
            │
            ├─> NTFS写入密文到磁盘
            │
            └─> TutorialPostWrite()
                 ├─> FltWriteFile() - 写入"TUTENC!!"标识
                 └─> ExFreePoolWithTag() - 释放缓冲区
```

## 学习重点标注

```
⭐⭐⭐ 必须掌握:
- TutorialDriver.c: 驱动注册流程
- TutorialRead.c: 透明解密的实现
- TutorialWrite.c: 透明加密的实现
- 缓冲区交换机制

⭐⭐ 重要理解:
- TutorialCreate.c: 文件上下文管理
- TutorialConfig.c: 进程和文件检查
- Pre/Post回调的配合

⭐ 基础知识:
- TutorialCipher.c: XOR算法（可替换为AES）
- Tutorial.inf: 驱动安装配置
```

---

**建议学习顺序**: 
1. 先看架构图理解整体流程
2. 再看具体场景（写入→读取）理解细节
3. 最后看API调用链理解实现方式
