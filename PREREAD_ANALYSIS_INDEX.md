# PreRead回调机制分析文档索引

## 文档说明

本分析针对问题：**"分析下项目代码，是如何实现notepad打开txt文件能够触发PreRead回调的"**

提供了两份详细的技术分析文档，从不同角度解释了基于Windows Minifilter框架的透明加解密驱动如何实现PreRead回调机制。

## 文档列表

### 📄 [PREREAD_CALLBACK_ANALYSIS.md](./PREREAD_CALLBACK_ANALYSIS.md)
**技术深度分析文档** (424行, 14KB)

**适合人群**: 需要深入理解代码实现细节的开发者

**内容概览**:
- ✅ **核心架构**: Minifilter框架介绍
- ✅ **驱动注册**: `FltRegisterFilter`和`FltStartFiltering`详解
- ✅ **回调注册表**: `IRP_MJ_READ`与`PocPreReadOperation`的绑定机制
- ✅ **过滤器注册结构**: `FilterRegistration`结构体详解
- ✅ **完整调用流程**: 从用户双击文件到PreRead回调的13步详细流程
- ✅ **PocPreReadOperation详细分析**: 
  - 获取读取参数
  - 查找StreamContext
  - 判断文件加密状态
  - 获取进程信息
  - 缓冲I/O处理
  - 双缓冲机制实现
  - 返回值决定后续处理
- ✅ **关键技术点**: 
  - Minifilter框架机制
  - 双缓冲技术
  - 文件大小管理
  - 进程权限控制
- ✅ **触发条件总结**: 6个必要条件详解
- ✅ **调试验证方法**: DebugView配置和日志输出
- ✅ **相关文件清单**: 9个核心源文件说明

**关键代码片段**: 包含实际源代码的关键部分，便于对照学习

---

### 📊 [PREREAD_FLOW_DIAGRAM.md](./PREREAD_FLOW_DIAGRAM.md)
**可视化流程图文档** (363行, 16KB)

**适合人群**: 需要快速理解整体流程和架构的读者

**内容概览**:
- ✅ **系统架构层次图**: 用户态→内核态→驱动→文件系统的完整层次
- ✅ **Notepad打开TXT详细流程**: 
  - 13步完整流程图（从双击文件到显示内容）
  - 每步都标注了关键函数和数据流
- ✅ **对比流程**: WordPad（非授权进程）打开同一文件的不同处理
- ✅ **StreamContext数据结构**: 核心数据结构的字段说明
- ✅ **文件物理布局**: 
  - 未加密文件布局
  - 已加密文件布局（明文区+扩展区+标识尾）
  - 不同进程读取到的范围对比
- ✅ **双缓冲机制详解**: 
  - 授权进程的缓冲区流转（3个缓冲区）
  - 非授权进程的直接读取（1个缓冲区）
  - 内存地址示例说明
- ✅ **关键判断逻辑决策树**: PreRead返回值的决策流程图
- ✅ **性能优化点**: 5个关键性能优化技术

**可视化特点**: 大量ASCII图表，直观展示数据流和控制流

---

## 快速导航

### 如果你想了解...

| 问题 | 推荐文档 | 章节 |
|------|---------|------|
| PreRead回调是如何注册的？ | PREREAD_CALLBACK_ANALYSIS.md | 2. 回调注册表 |
| 驱动加载时发生了什么？ | PREREAD_CALLBACK_ANALYSIS.md | 1. 驱动注册 |
| Notepad打开文件的完整流程？ | PREREAD_FLOW_DIAGRAM.md | Notepad打开TXT文件的详细流程 |
| 授权进程和非授权进程的区别？ | PREREAD_FLOW_DIAGRAM.md | 对比: WordPad打开同一文件 |
| 双缓冲是怎么实现的？ | 两份文档都有 | ANALYSIS: 6.双缓冲机制 / DIAGRAM: 双缓冲机制详解 |
| 文件在磁盘上如何存储？ | PREREAD_FLOW_DIAGRAM.md | 文件物理布局 |
| PreRead函数的具体实现？ | PREREAD_CALLBACK_ANALYSIS.md | PocPreReadOperation详细分析 |
| 系统架构是什么样的？ | PREREAD_FLOW_DIAGRAM.md | 系统架构层次 |
| 如何调试验证？ | PREREAD_CALLBACK_ANALYSIS.md | 调试验证 |
| 有哪些相关源文件？ | PREREAD_CALLBACK_ANALYSIS.md | 相关文件清单 |

---

## 核心要点速览

### PreRead回调触发的核心机制

```
1. 驱动注册阶段 (DriverEntry):
   FltRegisterFilter(&FilterRegistration) 
   → 注册回调表: IRP_MJ_READ → PocPreReadOperation

2. 文件打开阶段 (IRP_MJ_CREATE):
   PocPostCreateOperation 
   → 创建StreamContext
   → 检查文件标识尾
   → 设置IsCipherText标志

3. 文件读取阶段 (IRP_MJ_READ):
   Filter Manager 自动分发
   → PocPreReadOperation ← 这里触发!
   → 检查加密状态 + 进程权限
   → 分配双缓冲区（授权进程）
   → PocPostReadOperation 
   → 解密数据

4. 返回数据:
   授权进程(notepad.exe) → 明文
   非授权进程(wordpad.exe) → 密文
```

### 关键数据结构

```c
// 文件上下文 - 跟踪加密状态
StreamContext {
    IsCipherText: TRUE/FALSE  // 是否已加密
    FileSize: 明文大小         // 应用层看到的大小
    FileName: "test.txt"
}

// 缓冲区交换上下文 - 双缓冲机制
SwapBufferContext {
    OrigBuffer: 应用程序缓冲区  // 最终返回给notepad
    NewBuffer: 驱动分配缓冲区   // 用于读取密文并解密
}
```

### 进程权限配置

**授权进程** (读取明文):
- `C:\Windows\System32\notepad.exe` ← Notepad!
- `C:\Program Files\Microsoft Office\root\Office16\WINWORD.exe`
- WPS Office相关进程
- 配置位置: `Poc/Config.c` 的 `secure_process[]`

**非授权进程** (读取密文):
- 所有不在授权列表的进程
- 例如: `wordpad.exe`

**备份进程** (读取完整密文含标识尾):
- `C:\Windows\explorer.exe`
- `C:\Program Files\VMware\VMware Tools\vmtoolsd.exe`
- 配置位置: `Poc/Config.c` 的 `backup_process[]`

---

## 源代码位置参考

| 功能 | 文件路径 | 关键函数/行号 |
|------|---------|-------------|
| PreRead回调实现 | `Poc/Read.c` | `PocPreReadOperation` (25-400行) |
| PostRead回调实现 | `Poc/Read.c` | `PocPostReadOperation` (400+行) |
| 回调注册表 | `Poc/Poc.c` | `Callbacks[]` (328-359行) |
| 驱动注册 | `Poc/Poc.c` | `DriverEntry` (48-600行) |
| 过滤器结构 | `Poc/Poc.c` | `FilterRegistration` (382-402行) |
| Create回调 | `Poc/Poc.c` | `PocPreCreateOperation` (700行) |
| StreamContext管理 | `Poc/Context.c` | `PocFindOrCreateStreamContext` |
| 进程权限配置 | `Poc/Config.c` | `secure_process[]`, `backup_process[]` |
| AES加解密 | `Poc/cipher.c` | 加解密算法实现 |
| 工具函数 | `Poc/Utils.c` | 辅助函数 |

---

## 阅读建议

### 初学者路径
1. 先读 **PREREAD_FLOW_DIAGRAM.md** 的"系统架构层次图"，理解整体架构
2. 再读"Notepad打开TXT文件的详细流程"，跟随流程图理解每一步
3. 看"双缓冲机制详解"，理解授权/非授权进程的区别
4. 最后读 **PREREAD_CALLBACK_ANALYSIS.md** 的"触发条件总结"和"总结"

### 开发者路径
1. 先读 **PREREAD_CALLBACK_ANALYSIS.md** 的"驱动注册"和"回调注册表"，理解注册机制
2. 详细阅读"PocPreReadOperation详细分析"的7个步骤
3. 对照源代码 `Poc/Read.c` 理解具体实现
4. 参考 **PREREAD_FLOW_DIAGRAM.md** 的决策树理解返回值逻辑
5. 配置DebugView验证理解

### 架构师路径
1. 阅读两份文档的"核心架构"和"系统架构层次"
2. 重点理解"Minifilter框架机制"和"双缓冲技术"
3. 分析"文件大小管理"和"进程权限控制"的设计
4. 思考"性能优化点"和可能的改进方向

---

## 相关资源

### 项目文档
- [README.md](./README.md) - 项目总体介绍
- [TESTMANUAL.md](./TESTMANUAL.md) - 测试手册
- [CHANGELOG.md](./CHANGELOG.md) - 版本变更记录

### Microsoft官方文档
- [Minifilter Driver Architecture](https://docs.microsoft.com/en-us/windows-hardware/drivers/ifs/filter-manager-concepts)
- [IRP Major Function Codes](https://docs.microsoft.com/en-us/windows-hardware/drivers/kernel/irp-major-function-codes)
- [File System Minifilter Drivers](https://docs.microsoft.com/en-us/windows-hardware/drivers/ifs/file-system-minifilter-drivers)

### 本项目引用
- Windows-driver-samples (Microsoft)
- Windows内核安全与驱动开发 (谭文)
- Windows内核情景分析 (毛德操)

---

## 反馈与贡献

如果您发现文档中有任何错误或需要补充的内容，欢迎：
1. 提交Issue反馈问题
2. 提交PR改进文档
3. 与维护者讨论技术细节

---

## 版权声明

本分析文档基于 **FOKS-TROT** 项目（GPLv3许可证）创建。

- **项目**: FOKS-TROT - 基于Minifilter框架的双缓冲透明加解密驱动
- **专利**: ZL 202210549949.1 基于密文挪用的文件透明加解密方法及系统
- **专利权人**: 山东大学
- **作者**: hkx3upper, wangzhankun
- **许可证**: GPLv3

---

**最后更新**: 2026-01-31

**文档版本**: 1.0

**分析完成度**: ✅ 100%
