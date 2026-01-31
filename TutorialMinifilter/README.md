# TutorialMinifilter - 教学版透明文件加解密驱动

## 快速开始

这是一个简化的Windows内核minifilter驱动，用于演示文件透明加解密的核心原理。

### 与完整版的区别

| 特性 | 完整版 (FOKS-TROT) | 教学版 (TutorialMinifilter) |
|------|-------------------|---------------------------|
| 代码行数 | ~5000行 | ~500行 |
| 文件数量 | 20+个文件 | 7个文件 |
| 加密算法 | AES-128 + 密文挪用 | 简单XOR |
| 授权进程 | 多个进程，复杂管理 | 只有notepad.exe |
| 文件类型 | txt/docx/xlsx/pptx等 | 只有.txt |
| 缓冲管理 | 双缓冲机制 | 简化的缓冲交换 |
| 文件标识 | 4KB复杂标识尾 | 8字节魔术字头 |
| 学习曲线 | 陡峭 | 平缓 |

### 核心原理

```
┌─────────────────────────────────────────────────────────┐
│                     透明加解密流程                        │
├─────────────────────────────────────────────────────────┤
│                                                         │
│  用户操作: notepad.exe 保存 test.txt                     │
│      ↓                                                  │
│  应用程序写入明文: "Hello World"                          │
│      ↓                                                  │
│  TutorialPreWrite 拦截                                  │
│      ├─ 复制明文到新缓冲区                               │
│      ├─ XOR加密: "Hello World" → 密文                    │
│      └─ 替换IRP缓冲区 = 密文缓冲区                        │
│      ↓                                                  │
│  文件系统写入磁盘: [TUTENC!!][密文数据]                  │
│      ↓                                                  │
│  ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━             │
│      ↓                                                  │
│  用户操作: notepad.exe 打开 test.txt                     │
│      ↓                                                  │
│  TutorialPostCreate                                     │
│      └─ 读取文件头: "TUTENC!!" → 标记为已加密            │
│      ↓                                                  │
│  应用程序读取文件                                        │
│      ↓                                                  │
│  TutorialPreRead 拦截                                   │
│      ├─ 检查: notepad.exe? 已加密?                       │
│      ├─ 分配新缓冲区                                     │
│      └─ 替换IRP缓冲区 = 新缓冲区                         │
│      ↓                                                  │
│  文件系统读取: 密文 → 新缓冲区                           │
│      ↓                                                  │
│  TutorialPostRead                                       │
│      ├─ XOR解密: 密文 → "Hello World"                   │
│      └─ 复制明文到应用程序缓冲区                          │
│      ↓                                                  │
│  notepad.exe 显示: "Hello World"                        │
│                                                         │
└─────────────────────────────────────────────────────────┘
```

## 文件说明

```
TutorialMinifilter/
│
├── Tutorial.h               # 头文件：结构体、常量、函数声明
│   └─ 定义：魔术字节、XOR密钥、文件上下文等
│
├── TutorialDriver.c         # 驱动主文件：注册和初始化
│   ├─ DriverEntry()        # 驱动入口点
│   ├─ TutorialUnload()     # 驱动卸载
│   └─ 回调注册表            # 定义拦截哪些I/O操作
│
├── TutorialCreate.c         # 文件创建/打开处理
│   ├─ TutorialPreCreate()  # 检查是否监控文件
│   └─ TutorialPostCreate() # 读取文件头，检查加密标识
│
├── TutorialRead.c           # 文件读取处理（核心！）
│   ├─ TutorialPreRead()    # 替换缓冲区，准备解密
│   └─ TutorialPostRead()   # 解密数据，复制给应用程序
│
├── TutorialWrite.c          # 文件写入处理（核心！）
│   ├─ TutorialPreWrite()   # 加密数据，替换缓冲区
│   └─ TutorialPostWrite()  # 写入加密标识
│
├── TutorialCipher.c         # 加密/解密算法
│   └─ TutorialXorEncryptDecrypt() # XOR加解密
│
├── TutorialConfig.c         # 配置和工具函数
│   ├─ TutorialIsAuthorizedProcess()  # 检查授权进程
│   ├─ TutorialIsTargetExtension()    # 检查文件扩展名
│   └─ TutorialIsMonitoredFile()      # 综合判断
│
├── Tutorial.inf             # 驱动安装配置文件
│
└── README.md               # 本文件
```

## 快速测试

### 1. 编译驱动（需要WDK）

打开Visual Studio，生成解决方案。

### 2. 开启测试模式

```cmd
# 管理员CMD
bcdedit /set testsigning on
# 重启
```

### 3. 安装驱动

```cmd
# 管理员CMD，进入编译输出目录
cd TutorialMinifilter\x64\Debug

# 安装
rundll32.exe setupapi.dll,InstallHinfSection DefaultInstall 132 完整路径\Tutorial.inf

# 启动
net start Tutorial
```

### 4. 测试

```cmd
# 1. 用notepad创建文件
notepad C:\test.txt
# 输入: Hello, this is a test!
# 保存并关闭

# 2. 用wordpad打开（非授权程序）
wordpad C:\test.txt
# 应该看到乱码（密文）

# 3. 再用notepad打开
notepad C:\test.txt
# 应该看到原文: Hello, this is a test!
```

### 5. 查看二进制

```cmd
# 十六进制查看文件
certutil -encodehex C:\test.txt hex.txt

# 文件开头应该是: 54 55 54 45 4E 43 21 21
# 这是 "TUTENC!!" 的ASCII码
```

### 6. 卸载

```cmd
# 停止
net stop Tutorial

# 卸载
rundll32.exe setupapi.dll,InstallHinfSection DefaultUninstall 132 完整路径\Tutorial.inf
```

## 学习路径

### 第1天：理解原理

1. 阅读主文档: `../TUTORIAL.md`
2. 理解透明加解密的概念
3. 理解缓冲区交换的原理

### 第2天：阅读驱动主文件

1. 打开 `TutorialDriver.c`
2. 理解 `DriverEntry()` 的流程
3. 理解回调注册表的作用
4. 理解 `FltRegisterFilter()` 和 `FltStartFiltering()`

### 第3天：阅读配置文件

1. 打开 `TutorialConfig.c`
2. 理解如何判断授权进程
3. 理解如何判断文件扩展名
4. 修改：添加 `notepad++.exe` 到授权列表

### 第4天：阅读Create回调

1. 打开 `TutorialCreate.c`
2. 理解 `PreCreate` 和 `PostCreate` 的区别
3. 理解文件上下文的作用
4. 理解如何检测文件加密标识

### 第5天：阅读Read回调（核心）

1. 打开 `TutorialRead.c`
2. **重点理解** `PreRead` 的缓冲区替换
3. **重点理解** `PostRead` 的解密流程
4. 理解 Pre 和 Post 之间如何传递信息

### 第6天：阅读Write回调（核心）

1. 打开 `TutorialWrite.c`
2. **重点理解** `PreWrite` 的加密流程
3. 理解 `PostWrite` 如何添加加密标识
4. 理解为何拒绝非授权进程写入

### 第7天：阅读加密算法

1. 打开 `TutorialCipher.c`
2. 理解XOR加密的原理
3. 尝试修改密钥，观察效果
4. （可选）研究如何替换为AES

## 调试技巧

### 启用内核日志

```
注册表编辑器:
HKEY_LOCAL_MACHINE\SYSTEM\CurrentControlSet\Control\Session Manager\Debug Print Filter

新建DWORD: "default" = 0x0000000F
重启
```

### 使用DebugView

1. 下载并运行DebugView（管理员）
2. Capture → Capture Kernel
3. 观察 `[Tutorial]` 开头的日志

### 常见日志

```
[Tutorial] DriverEntry: 教学版透明加解密驱动开始加载...
[Tutorial] DriverEntry: 过滤器注册成功
[Tutorial] DriverEntry: 驱动加载成功，开始过滤I/O操作

[Tutorial] PreCreate: 文件 = \Device\HarddiskVolume3\test.txt
[Tutorial] PreCreate: 监控文件，需要PostCreate检查加密状态
[Tutorial] PostCreate: 文件打开成功
[Tutorial] PostCreate: 检测到加密标识，文件已加密

[Tutorial] PreRead: 授权进程读取加密文件，准备解密
[Tutorial] PreRead: 读取长度 = 1024 字节
[Tutorial] PreRead: 缓冲区替换完成，等待PostRead解密
[Tutorial] PostRead: 成功读取 1024 字节密文
[Tutorial] PostRead: 解密 1016 字节（跳过魔术字节）
[Tutorial] PostRead: 复制明文到应用程序缓冲区
[Tutorial] PostRead: 解密完成，资源已清理

[Tutorial] PreWrite: 授权进程写入，准备加密
[Tutorial] PreWrite: 写入长度 = 512 字节, 偏移 = 0
[Tutorial] PreWrite: 加密 512 字节完成
[Tutorial] PreWrite: 缓冲区替换完成，密文将写入磁盘
[Tutorial] PostWrite: 成功写入 512 字节
[Tutorial] PostWrite: 写入加密标识
[Tutorial] PostWrite: 加密标识写入成功
```

## 实验练习

### 练习1：修改授权进程

修改 `TutorialConfig.c`，添加 `notepad++.exe` 到授权列表。

### 练习2：支持.log文件

修改 `TutorialConfig.c`，添加 `.log` 到监控扩展名列表。

### 练习3：修改加密密钥

修改 `Tutorial.h` 中的 `TUTORIAL_XOR_KEY`，观察已加密文件无法解密。

### 练习4：添加日志

在关键位置添加更多 `TutorialDbgPrint()`，观察驱动执行流程。

### 练习5：简单的密钥管理

修改代码，使每个文件使用不同的密钥（提示：可以基于文件名生成）。

## 进阶学习

完成教学版学习后，可以研究完整版（FOKS-TROT）的高级特性：

1. **AES加密**: 学习如何使用Windows CNG库实现AES加密
2. **密文挪用**: 避免明文必须分块对齐的技术
3. **双缓冲机制**: 更复杂但更高效的缓冲管理
4. **进程权限管理**: 授权/非授权/备份进程的分类
5. **文件标识尾**: 4KB的完整元数据管理
6. **Office文件支持**: 处理docx等使用临时文件的格式
7. **重入机制**: 特权加密/解密的实现
8. **IRQL管理**: 避免蓝屏的关键技术

## 常见问题

**Q: 为什么用XOR而不是AES？**

A: 教学目的。XOR简单易懂，便于调试。实际产品必须使用AES等安全算法。

**Q: 可以用于生产环境吗？**

A: **不可以**！这只是教学版，缺少很多安全特性。请使用完整版。

**Q: 驱动无法加载？**

A: 检查：
1. 是否开启测试模式？
2. 是否以管理员身份运行？
3. WDK是否正确安装？

**Q: notepad打开仍是乱码？**

A: 检查：
1. DebugView中是否有日志？
2. 驱动是否正常运行？（`sc query Tutorial`）
3. 进程名是否匹配？（必须是 `notepad.exe`，不能是 `notepad++.exe`）

## 许可证

本教学版代码遵循GPLv3许可证，与完整版FOKS-TROT保持一致。

## 反馈

如有问题或建议，请提交Issue到主仓库。

---

**祝学习顺利！** 🎓

如果本教程对您有帮助，请给项目点个Star ⭐
