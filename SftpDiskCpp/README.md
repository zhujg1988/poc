# SftpDiskCpp

C++ implementation of an SFTP virtual disk for Windows, using:

- **[WinFSP](https://winfsp.dev/)** — Windows File System Proxy (user-mode filesystem driver)
- **[libssh2](https://libssh2.org/)** — portable SSH / SFTP client library

It is a functional equivalent of the C# `SftpDisk` project in this repository,
re-implemented entirely in C++17.

---

## Architecture

```
Windows application / Explorer
          │
          ▼ (NTFS-compatible file operations)
      WinFSP kernel driver
          │
          ▼ (virtual callbacks via winfsp.hpp Fsp::FileSystem)
  SftpFileSystem (this project)
          │
          ▼ (libssh2 SFTP commands over TCP)
      Remote SFTP server
```

Key design decisions compared to the C# version:

| Concern | C# (SSH.NET) | C++ (libssh2) |
|---|---|---|
| Truncate/extend file | Read+rewrite entire retained content | `SSH_FXP_FSETSTAT(ATTR_SIZE)` directly on server |
| Volume statistics | Synthetic 1 TiB | `statvfs` query; fall back to synthetic |
| Threading | WinFSP multi-thread → single mutex | Same: one `std::mutex` for the libssh2 session |
| Directory listing | Cached per-open-handle | Cached per-open-handle |

---

## Prerequisites

| Tool | Notes |
|---|---|
| Visual Studio 2022 | C++ Desktop workload required |
| [WinFSP](https://github.com/winfsp/winfsp/releases) | Install the MSI; sets `WINFSP_DIR` env var |
| [vcpkg](https://vcpkg.io/) | Package manager for libssh2 |
| Administrator privileges | Required at runtime to mount a drive letter |

---

## Build

### 1. Install vcpkg (if not already)

```powershell
git clone https://github.com/microsoft/vcpkg.git C:\vcpkg
C:\vcpkg\bootstrap-vcpkg.bat -disableMetrics
C:\vcpkg\vcpkg.exe integrate install
```

### 2. Open the solution in Visual Studio 2022

Open `Poc.sln`.  vcpkg manifest mode will automatically install `libssh2`
(and its OpenSSL dependency) into the project's `vcpkg_installed\` directory
on the first build.

### 3. Build `SftpDiskCpp`

Select **Release | x64** and build.  The output binary is placed in
`x64\Release\SftpDiskCpp.exe`.

---

## Usage

Run from an **elevated** (Administrator) command prompt:

```powershell
# Password authentication
SftpDiskCpp.exe --host sftp.example.com --user alice --password s3cr3t --mount Z:

# Private-key authentication (PEM format, no passphrase)
SftpDiskCpp.exe --host sftp.example.com --user alice --key C:\keys\id_rsa --mount Z:

# Mount a sub-directory of the server
SftpDiskCpp.exe --host sftp.example.com --user alice --password s3cr3t \
    --path /home/alice/data --mount Z:
```

The drive stays mounted until you press **Ctrl+C**.

### All options

| Option | Default | Description |
|---|---|---|
| `--host <host>` | — | SFTP server hostname or IP |
| `--port <port>` | `22` | SSH port |
| `--user <user>` | — | SSH username |
| `--password <pass>` | — | Password authentication |
| `--key <pem-file>` | — | PEM private key (no passphrase) |
| `--mount <X:>` | — | Drive letter to mount |
| `--path <dir>` | `/` | Remote root directory |

---

## Limitations (PoC)

- **No passphrase support** for private key files (extend `libssh2_userauth_publickey_fromfile` call).
- **Single SSH session / single mutex** — all SFTP operations are serialised;
  sufficient for interactive use but not for high-throughput bulk transfers.
- **Directory listing cached per handle** — changes made by other clients while
  a directory is open may not be visible until the directory is reopened.
- **No symlink transparency** — symlinks appear as regular files/directories as
  reported by the server's `stat` response.
