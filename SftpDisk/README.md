# SftpDisk

基于 **WinFSP** 的 SFTP 虚拟磁盘挂载工具。

通过 [WinFSP](https://winfsp.dev) 用户态文件系统框架，将远端 SFTP 服务器的目录挂载为本地 Windows 盘符，实现透明的文件读写、目录浏览、重命名、删除等操作。

---

## 架构

```
Windows 应用程序
    │
    ▼
WinFSP 内核驱动 (winfsp.sys)
    │ 文件系统 IRP 回调
    ▼
SftpDisk.exe  ──  SftpFileSystem (FileSystemBase)
                        │ SSH.NET SftpClient
                        ▼
                  远端 SFTP 服务器
```

| 组件 | 职责 |
|------|------|
| `Program.cs` | 命令行参数解析、SFTP 连接、WinFSP 主机生命周期管理 |
| `SftpFileSystem.cs` | 继承 `Fsp.FileSystemBase`，将所有 WinFSP 回调映射为 SSH.NET SFTP 操作 |
| `SftpContext.cs` | 每个打开文件/目录的句柄状态（远端路径 + `SftpFileStream`） |

### 已实现的 WinFSP 回调

- `GetVolumeInfo` — 返回卷信息（1 TiB 虚拟空间）
- `GetSecurityByName` — 根据文件名获取属性，用于 Windows 路径解析
- `Create` / `Open` / `Overwrite` — 创建、打开、覆写文件或目录
- `Cleanup` / `Close` — 清理句柄，处理延迟删除
- `Read` / `Write` / `Flush` — 文件数据读写
- `GetFileInfo` / `SetBasicInfo` / `SetFileSize` — 元数据管理
- `CanDelete` / `Rename` — 删除检查与重命名
- `GetSecurity` / `SetSecurity` — 安全描述符（SFTP 不支持，静默忽略）
- `ReadDirectoryEntry` — 目录枚举（支持 marker 分页）

---

## 前置条件

1. **WinFSP 已安装**（含内核驱动）：<https://winfsp.dev/rel/>  
   安装完成后 `WinFsp-MSIL.dll` 位于  
   `C:\Program Files (x86)\WinFsp\bin\WinFsp-MSIL.dll`

2. **以管理员身份运行**（挂载文件系统需要管理员权限）。

3. 目标盘符当前未被占用。

---

## 编译

```
# 需要 .NET SDK 6+ 以及 WinFSP 已安装（提供 WinFsp-MSIL.dll）
cd SftpDisk
dotnet build -c Release
```

若 WinFSP 安装在非默认路径，请在 `SftpDisk.csproj` 中修改  
`<WinFspMsilDll>` 属性指向正确位置。

---

## 用法

```
SftpDisk.exe --host <host> [--port <port>] --user <user>
             (--password <pass> | --key <key-file>)
             --mount <X:> [--path <remote-root>]
```

| 参数 | 说明 |
|------|------|
| `--host` | SFTP 服务器主机名或 IP |
| `--port` | 端口（默认 22） |
| `--user` | SSH 用户名 |
| `--password` | 密码认证 |
| `--key` | PEM 私钥文件路径（密钥认证） |
| `--mount` | 本地挂载盘符，如 `Z:` |
| `--path` | 远端根目录（默认 `/`） |

### 示例

```cmd
# 密码认证，挂载远端 /data 到 Z:
SftpDisk.exe --host 192.168.1.10 --user alice --password s3cr3t --mount Z: --path /data

# 密钥认证，挂载远端根目录到 Y:
SftpDisk.exe --host myserver.local --user bob --key %USERPROFILE%\.ssh\id_rsa --mount Y:
```

按 **Ctrl+C** 卸载磁盘并退出程序。

---

## 限制与待改进

- 不支持符号链接（SFTP 扩展功能，SSH.NET 未完整暴露）
- 安全描述符（ACL）静默忽略，所有用户可见所有文件
- SetFileSize 截断大文件时需要将内容读入内存后重写，建议后续改用  
  SFTP `SSH_FXP_FSETSTAT` + `SSH_FILEXFER_ATTR_SIZE` 直接操作
- 写操作每次都调用 `GetAttributes` 刷新元数据，高频写场景可考虑缓存

---

## 许可

与主项目相同，遵循 **GPLv3** 协议。  
依赖库 [SSH.NET](https://github.com/sshnet/SSH.NET) 遵循 MIT 协议，  
[WinFSP](https://github.com/billziss-gh/winfsp) 遵循 GPLv3 协议。
