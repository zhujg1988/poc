# 透明文件加解密教学版 - Tutorial Minifilter

## 概述

本教学版是一个简化的Windows内核驱动程序，演示了如何使用Minifilter框架实现文件的透明加解密。

### 什么是透明加解密？

**透明加解密**是指：
- 文件在磁盘上以加密形式存储
- 授权应用程序（如notepad.exe）读取文件时，驱动自动解密，应用程序看到的是明文
- 授权应用程序写入文件时，驱动自动加密，保存到磁盘的是密文
- 非授权应用程序读取文件时，看到的是密文（乱码）
- **应用程序无需任何修改**，完全不知道加解密的存在

## 教学版特点

本教学版相比完整版（FOKS-TROT）做了以下简化：

### 1. 简化的加密算法
- **完整版**: AES-128 ECB + 密文挪用技术
- **教学版**: 简单的XOR加密（密钥：0x42）
- **原因**: XOR算法简单易懂，便于学习Minifilter机制，不会被加密算法复杂度干扰

### 2. 简化的进程控制
- **完整版**: 复杂的授权/非授权/备份进程管理
- **教学版**: 只有notepad.exe是授权进程
- **原因**: 专注于核心的读写拦截机制

### 3. 简化的文件类型
- **完整版**: 支持txt, docx, xlsx, pptx等多种格式
- **教学版**: 只支持.txt文件
- **原因**: 减少边界条件，专注核心流程

### 4. 简化的文件标识
- **完整版**: 4KB的加密标识尾，包含完整的元数据
- **教学版**: 简单的8字节魔术字头 "TUTENC!!"
- **原因**: 降低复杂度，便于调试

### 5. 大幅简化的代码
- **完整版**: ~5000行代码，20+个源文件
- **教学版**: ~500行代码，5个源文件
- **原因**: 去除高级特性，保留核心逻辑

## 核心原理

### 工作流程图

```
用户操作: notepad.exe打开test.txt
         ↓
    CreateFile()
         ↓
系统生成 IRP_MJ_CREATE 请求
         ↓
Minifilter拦截: TutorialPreCreate
         ↓
检查: 是否.txt文件？是否在监控目录？
         ↓
系统打开文件
         ↓
Minifilter: TutorialPostCreate
         ↓
读取文件开头8字节，检查是否有"TUTENC!!"标记
         ↓
如果有标记 → 设置标志: 此文件已加密
         ↓
返回文件句柄给notepad.exe
         ↓
━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
用户操作: notepad.exe读取文件内容
         ↓
    ReadFile()
         ↓
系统生成 IRP_MJ_READ 请求
         ↓
Minifilter拦截: TutorialPreRead
         ↓
检查: 当前进程是notepad.exe吗？
      文件已加密吗？
         ↓
是 → 分配新缓冲区，记录原始缓冲区地址
否 → 直接放行，返回密文
         ↓
系统从磁盘读取密文数据到新缓冲区
         ↓
Minifilter: TutorialPostRead
         ↓
如果是notepad.exe:
    对新缓冲区数据进行XOR解密
    复制解密后的数据到原始缓冲区
    释放新缓冲区
         ↓
返回数据给notepad.exe（明文）
         ↓
notepad.exe显示明文内容
━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
用户操作: notepad.exe修改并保存文件
         ↓
    WriteFile()
         ↓
系统生成 IRP_MJ_WRITE 请求
         ↓
Minifilter拦截: TutorialPreWrite
         ↓
检查: 当前进程是notepad.exe吗？
         ↓
是 → 分配新缓冲区
    对数据进行XOR加密
    替换为加密后的缓冲区
否 → 拒绝写入（保护密文）
         ↓
系统将加密数据写入磁盘
         ↓
Minifilter: TutorialPostWrite
         ↓
如果是新文件 → 在开头写入"TUTENC!!"标记
         ↓
释放临时缓冲区
         ↓
完成
```

## 目录结构

```
TutorialMinifilter/
├── TutorialDriver.c      # 驱动主文件：注册、初始化
├── TutorialRead.c        # 读取拦截：PreRead、PostRead
├── TutorialWrite.c       # 写入拦截：PreWrite、PostWrite
├── TutorialCipher.c      # 加密算法：XOR加密/解密
├── TutorialConfig.c      # 配置：授权进程、文件扩展名
├── Tutorial.h            # 头文件：结构体、函数声明
├── Tutorial.inf          # 驱动安装配置
└── Tutorial.vcxproj      # Visual Studio项目文件
```

## 关键概念讲解

### 1. Minifilter框架

Minifilter是Windows内核提供的文件系统过滤驱动框架：

- **位置**: 位于应用程序和文件系统驱动之间
- **作用**: 拦截I/O操作（读、写、创建等）
- **优势**: 微软提供统一的管理和优化

### 2. Pre/Post回调

每个I/O操作都有两个拦截点：

**PreOperation** (操作前):
- 在文件系统执行实际操作**之前**被调用
- 可以修改操作参数（如缓冲区地址）
- 可以决定是否继续操作

**PostOperation** (操作后):
- 在文件系统执行实际操作**之后**被调用
- 可以修改返回的数据
- 可以获取操作结果

### 3. 缓冲区交换（Buffer Swapping）

透明加解密的核心技术：

```c
// PreRead时:
原始缓冲区 = 应用程序提供的缓冲区
新缓冲区 = 驱动分配的缓冲区
将IRP的缓冲区指针 → 指向新缓冲区

// 文件系统读取密文到新缓冲区

// PostRead时:
解密(新缓冲区)
复制(新缓冲区 → 原始缓冲区)
释放(新缓冲区)
// 应用程序从原始缓冲区读到明文
```

### 4. 文件加密标识

如何知道一个文件是否已加密？

- 在文件开头写入魔术字节："TUTENC!!" (8字节)
- 文件打开时（PostCreate）读取前8字节检查
- 如果有标记 → 文件已加密，设置标志
- 读写时根据此标志决定是否加解密

## 编译环境

### 必需工具

1. **Windows 10/11 x64** (推荐Windows 10)
2. **Visual Studio 2019/2022**
   - 必须安装"使用C++的桌面开发"工作负载
3. **Windows Driver Kit (WDK)**
   - 下载地址: https://docs.microsoft.com/en-us/windows-hardware/drivers/download-the-wdk
   - 版本要匹配Windows版本

### 编译步骤

1. 打开Visual Studio
2. 文件 → 打开 → 项目/解决方案
3. 选择 `TutorialMinifilter/Tutorial.vcxproj`
4. 选择配置: `Debug` 或 `Release`
5. 选择平台: `x64`
6. 生成 → 生成解决方案

编译输出: `x64/Debug/Tutorial.sys` 或 `x64/Release/Tutorial.sys`

## 安装与测试

### 前置条件

**开启测试模式** (必须！否则无法加载未签名驱动):

```cmd
# 以管理员身份运行CMD
bcdedit /set testsigning on
# 重启计算机
```

重启后，桌面右下角会显示"测试模式"水印。

### 安装驱动

方法1: 使用INF文件 (推荐)

```cmd
# 以管理员身份运行CMD
cd TutorialMinifilter\x64\Debug

# 安装
rundll32.exe setupapi.dll,InstallHinfSection DefaultInstall 132 完整路径\Tutorial.inf

# 启动
net start Tutorial
```

方法2: 使用OSR Loader

1. 下载OSR Loader: https://www.osronline.com/article.cfm%5Earticle=157.htm
2. 以管理员身份运行
3. 选择Tutorial.sys文件
4. Service Type: File System
5. 点击Register Service
6. 点击Start Service

### 测试步骤

**测试1: 验证驱动加载**

```cmd
# 查看驱动是否运行
sc query Tutorial

# 应该显示 STATE: 4 RUNNING
```

**测试2: 加密功能测试**

1. 创建测试目录: `C:\TestEncrypt`
2. 用notepad.exe创建文件: `C:\TestEncrypt\test.txt`
3. 输入一些文本: "Hello, this is a test!"
4. 保存并关闭

**测试3: 验证文件已加密**

1. 用WordPad（非授权程序）打开 `C:\TestEncrypt\test.txt`
2. 应该看到乱码（密文），不是原始文本
3. 这证明文件在磁盘上是加密的

**测试4: 验证透明解密**

1. 再次用notepad.exe打开 `C:\TestEncrypt\test.txt`
2. 应该看到原始文本: "Hello, this is a test!"
3. 这证明notepad读取时自动解密了

**测试5: 查看二进制内容**

```cmd
# 用十六进制工具查看文件
certutil -encodehex C:\TestEncrypt\test.txt test_hex.txt

# 文件开头应该是: 54 55 54 45 4E 43 21 21  ("TUTENC!!")
# 后续内容应该是加密后的数据
```

### 卸载驱动

```cmd
# 停止驱动
net stop Tutorial

# 卸载
rundll32.exe setupapi.dll,InstallHinfSection DefaultUninstall 132 完整路径\Tutorial.inf
```

## 调试技巧

### 1. 启用内核日志

```
注册表编辑器:
HKEY_LOCAL_MACHINE\SYSTEM\CurrentControlSet\Control\Session Manager\Debug Print Filter

新建DWORD值: "default"
设置为: 0x0000000F
重启计算机
```

### 2. 使用DebugView

1. 下载DebugView: https://docs.microsoft.com/en-us/sysinternals/downloads/debugview
2. 以管理员身份运行
3. Capture → Capture Kernel
4. 可以看到驱动的DbgPrint输出

### 3. WinDbg内核调试

配置双机调试（超出本教程范围）

## 常见问题

### Q1: 驱动无法加载，错误577?

**A**: 未开启测试模式。执行 `bcdedit /set testsigning on` 并重启。

### Q2: 编译错误：找不到fltKernel.h?

**A**: 未安装WDK或WDK路径配置错误。

### Q3: 蓝屏死机（BSOD）?

**A**: 
- 检查代码中的内存访问
- 确保在正确的IRQL级别调用API
- 使用WinDbg分析dump文件

### Q4: notepad打开文件仍是乱码?

**A**:
- 检查notepad.exe路径是否正确配置
- 检查文件扩展名是否为.txt
- 检查驱动是否正在运行

### Q5: 为什么选择XOR加密？

**A**: XOR仅用于教学演示，实际产品应使用AES等安全算法。本教程重点是Minifilter机制，而非加密算法。

## 代码详解

### TutorialDriver.c

```c
// 核心流程:
DriverEntry()
  ├─ FltRegisterFilter()      // 注册过滤器
  ├─ FltStartFiltering()      // 开始过滤
  └─ 返回成功

Unload()
  └─ FltUnregisterFilter()    // 卸载过滤器

// 关键数据结构:
FLT_REGISTRATION FilterRegistration = {
    .Callbacks = Callbacks,   // 回调函数数组
    // 定义了哪些I/O操作会被拦截
}

FLT_OPERATION_REGISTRATION Callbacks[] = {
    { IRP_MJ_CREATE, 0, PreCreate, PostCreate },
    { IRP_MJ_READ,   0, PreRead,   PostRead   },
    { IRP_MJ_WRITE,  0, PreWrite,  PostWrite  },
    { IRP_MJ_OPERATION_END }
}
```

### TutorialRead.c

```c
PreRead(IRP):
  如果 (进程 != notepad.exe) 返回  // 非授权进程看密文
  如果 (文件未加密) 返回
  
  分配新缓冲区
  保存原始缓冲区地址
  替换IRP缓冲区 = 新缓冲区
  返回 CALLBACK_NEEDED  // 需要PostRead

PostRead(IRP, Context):
  从新缓冲区读取密文
  XOR解密 → 明文
  复制明文到原始缓冲区
  释放新缓冲区
  返回
```

### TutorialWrite.c

```c
PreWrite(IRP):
  如果 (进程 != notepad.exe) 拒绝  // 保护密文
  
  分配新缓冲区
  从原始缓冲区复制明文
  XOR加密 → 密文
  替换IRP缓冲区 = 新缓冲区(密文)
  返回 CALLBACK_NEEDED

PostWrite(IRP, Context):
  如果是新文件:
    在文件开头写入"TUTENC!!"标记
  释放新缓冲区
  返回
```

## 学习路径建议

### 初学者 (1-2周)

1. **第1-2天**: 阅读本文档，理解透明加解密原理
2. **第3-4天**: 搭建编译环境，成功编译驱动
3. **第5-6天**: 安装驱动，完成全部测试
4. **第7-10天**: 阅读TutorialDriver.c，理解驱动注册
5. **第11-14天**: 阅读Read.c和Write.c，理解缓冲区交换

### 进阶学习 (2-4周)

1. 修改加密密钥，验证加解密流程
2. 添加对.log文件的支持
3. 改进文件标识（加入文件大小、加密时间等）
4. 实现简单的密钥管理
5. 添加更多授权进程（如notepad++.exe）

### 深入研究 (1-3个月)

1. 研究完整版FOKS-TROT的高级特性
2. 实现AES加密替代XOR
3. 实现双缓冲机制
4. 添加进程权限管理
5. 研究密文挪用技术

## 扩展阅读

### 官方文档

- [Minifilter Driver Programming](https://docs.microsoft.com/en-us/windows-hardware/drivers/ifs/filter-manager-concepts)
- [File System Minifilter Drivers](https://docs.microsoft.com/en-us/windows-hardware/drivers/ifs/file-system-minifilter-drivers)
- [IRP Major Function Codes](https://docs.microsoft.com/en-us/windows-hardware/drivers/kernel/irp-major-function-codes)

### 推荐书籍

- 《Windows内核安全与驱动开发》- 谭文
- 《Windows内核情景分析》- 毛德操
- 《Windows NT File System Internals》

### 相关项目

- **完整版**: FOKS-TROT (本仓库)
- **Microsoft Samples**: Windows-driver-samples/filesys/minifilter
- **开源项目**: EncryptedFilesystem, MiniSpy

## 贡献与反馈

如果您在学习过程中有任何问题或建议：

1. 提交Issue描述问题
2. 提交PR改进代码
3. 分享您的学习心得

## 许可证

本教学版代码遵循GPLv3许可证，与完整版FOKS-TROT保持一致。

---

**祝学习愉快！** 🎓

如果本教程帮助到您，请给项目点个Star ⭐
