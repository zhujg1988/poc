using System;
using System.Collections.Generic;
using System.IO;
using System.Runtime.InteropServices;
using Fsp;
using Renci.SshNet;
using Renci.SshNet.Common;

// Alias WinFSP's FileInfo struct to avoid collision with System.IO.FileInfo.
using FspFileInfo = Fsp.FileInfo;

namespace SftpDisk
{
    /// <summary>
    /// WinFSP file-system implementation that proxies all operations to a remote SFTP server
    /// via SSH.NET.  Extends <see cref="FileSystemBase"/> so WinFSP can call the virtual-disk
    /// callbacks from its kernel dispatcher threads.
    /// </summary>
    public sealed class SftpFileSystem : FileSystemBase
    {
        // ── fields ──────────────────────────────────────────────────────────

        private readonly SftpClient _sftp;

        /// <summary>Absolute root path on the SFTP server (no trailing slash).</summary>
        private readonly string _rootPath;

        /// <summary>Synthetic total volume size reported to Windows (1 TiB).</summary>
        private const ulong TotalSize = 1UL << 40;

        // ── construction ────────────────────────────────────────────────────

        public SftpFileSystem(SftpClient sftp, string rootPath)
        {
            _sftp = sftp ?? throw new ArgumentNullException(nameof(sftp));
            _rootPath = rootPath.TrimEnd('/');
        }

        // ── helpers ─────────────────────────────────────────────────────────

        /// <summary>
        /// Converts a Windows-style absolute path (backslash, starts with '\') to a
        /// POSIX-style path on the remote server rooted at <see cref="_rootPath"/>.
        /// </summary>
        private string ToRemotePath(string windowsPath)
        {
            var posix = windowsPath.Replace('\\', '/');
            var full = _rootPath + posix;
            return full.Length == 0 ? "/" : full;
        }

        /// <summary>
        /// Builds a WinFSP <see cref="FspFileInfo"/> from SFTP file attributes.
        /// SFTP does not carry a dedicated creation-time field, so we fall back to
        /// the last-write time for CreationTime and ChangeTime.
        /// </summary>
        private static FspFileInfo MakeFileInfo(
            long size,
            bool isDirectory,
            DateTime lastAccess,
            DateTime lastWrite)
        {
            var fi = new FspFileInfo();
            fi.FileAttributes = isDirectory
                ? (uint)FileAttributes.Directory
                : (uint)FileAttributes.Normal;
            fi.FileSize = (ulong)Math.Max(0, size);
            // Round allocation size up to a 4 KiB boundary.
            fi.AllocationSize = (fi.FileSize + 4095UL) & ~4095UL;
            fi.CreationTime   = (ulong)lastWrite.ToFileTimeUtc();
            fi.LastAccessTime = (ulong)lastAccess.ToFileTimeUtc();
            fi.LastWriteTime  = (ulong)lastWrite.ToFileTimeUtc();
            fi.ChangeTime     = fi.LastWriteTime;
            fi.IndexNumber    = 0;
            fi.HardLinks      = 0;
            return fi;
        }

        private FspFileInfo QueryFileInfo(string remotePath, bool isDirectory)
        {
            var attrs = _sftp.GetAttributes(remotePath);
            return MakeFileInfo(attrs.Size, isDirectory, attrs.LastAccessTime, attrs.LastWriteTime);
        }

        /// <summary>Maps SSH.NET / IO exceptions to NTSTATUS codes.</summary>
        private static int NtStatusFromException(Exception ex)
        {
            switch (ex)
            {
                case SftpPathNotFoundException _:
                    return STATUS_OBJECT_NAME_NOT_FOUND;
                case SftpPermissionDeniedException _:
                case UnauthorizedAccessException _:
                    return STATUS_ACCESS_DENIED;
                case DirectoryNotFoundException _:
                    return STATUS_OBJECT_PATH_NOT_FOUND;
                default:
                    return STATUS_UNEXPECTED_IO_ERROR;
            }
        }

        // ── FileSystemBase overrides ────────────────────────────────────────

        public override int GetVolumeInfo(out VolumeInfo volumeInfo)
        {
            volumeInfo = default;
            volumeInfo.TotalSize = TotalSize;
            volumeInfo.FreeSize  = TotalSize / 2; // synthetic free space
            volumeInfo.SetVolumeLabel("SFTP");
            return STATUS_SUCCESS;
        }

        public override int GetSecurityByName(
            string fileName,
            out uint fileAttributes,
            ref byte[]? securityDescriptor)
        {
            fileAttributes = 0;
            try
            {
                var path  = ToRemotePath(fileName);
                var attrs = _sftp.GetAttributes(path);
                fileAttributes = attrs.IsDirectory
                    ? (uint)FileAttributes.Directory
                    : (uint)FileAttributes.Normal;
                return STATUS_SUCCESS;
            }
            catch (Exception ex)
            {
                return NtStatusFromException(ex);
            }
        }

        public override int Create(
            string fileName,
            uint createOptions,
            uint grantedAccess,
            uint fileAttributes,
            byte[]? securityDescriptor,
            ulong allocationSize,
            out object? fileNode,
            out object? fileDesc,
            out FspFileInfo fileInfo,
            out string? normalizedName)
        {
            fileNode       = null;
            fileDesc       = null;
            fileInfo       = default;
            normalizedName = fileName;
            try
            {
                var path        = ToRemotePath(fileName);
                bool isDirectory = (createOptions & FILE_DIRECTORY_FILE) != 0;
                SftpContext ctx;
                if (isDirectory)
                {
                    _sftp.CreateDirectory(path);
                    ctx  = new SftpContext(path, true, null);
                    var attrs = _sftp.GetAttributes(path);
                    fileInfo = MakeFileInfo(0, true, attrs.LastAccessTime, attrs.LastWriteTime);
                }
                else
                {
                    var stream = _sftp.Open(path, FileMode.Create, FileAccess.ReadWrite);
                    ctx      = new SftpContext(path, false, stream);
                    fileInfo = MakeFileInfo(0, false, DateTime.UtcNow, DateTime.UtcNow);
                }
                fileNode = ctx;
                fileDesc = ctx;
                return STATUS_SUCCESS;
            }
            catch (Exception ex)
            {
                return NtStatusFromException(ex);
            }
        }

        public override int Open(
            string fileName,
            uint createOptions,
            uint grantedAccess,
            out object? fileNode,
            out object? fileDesc,
            out FspFileInfo fileInfo,
            out string? normalizedName)
        {
            fileNode       = null;
            fileDesc       = null;
            fileInfo       = default;
            normalizedName = fileName;
            try
            {
                var path  = ToRemotePath(fileName);
                var attrs = _sftp.GetAttributes(path);
                SftpContext ctx;
                if (attrs.IsDirectory)
                {
                    ctx      = new SftpContext(path, true, null);
                    fileInfo = MakeFileInfo(0, true, attrs.LastAccessTime, attrs.LastWriteTime);
                }
                else
                {
                    // Try read-write first; fall back to read-only for write-protected files.
                    SftpFileStream stream;
                    try
                    {
                        stream = _sftp.Open(path, FileMode.Open, FileAccess.ReadWrite);
                    }
                    catch (Exception ex) when (ex is UnauthorizedAccessException || ex is SftpPermissionDeniedException)
                    {
                        stream = _sftp.Open(path, FileMode.Open, FileAccess.Read);
                    }
                    ctx      = new SftpContext(path, false, stream);
                    fileInfo = MakeFileInfo(attrs.Size, false, attrs.LastAccessTime, attrs.LastWriteTime);
                }
                fileNode = ctx;
                fileDesc = ctx;
                return STATUS_SUCCESS;
            }
            catch (Exception ex)
            {
                return NtStatusFromException(ex);
            }
        }

        public override int Overwrite(
            object fileNode,
            object fileDesc,
            uint fileAttributes,
            bool replaceFileAttributes,
            ulong allocationSize,
            out FspFileInfo fileInfo)
        {
            fileInfo = default;
            var ctx = (SftpContext)fileDesc;
            try
            {
                ctx.Stream?.Close();
                ctx.Stream = _sftp.Open(ctx.RemotePath, FileMode.Truncate, FileAccess.ReadWrite);
                fileInfo   = MakeFileInfo(0, false, DateTime.UtcNow, DateTime.UtcNow);
                return STATUS_SUCCESS;
            }
            catch (Exception ex)
            {
                return NtStatusFromException(ex);
            }
        }

        public override void Cleanup(object fileNode, object fileDesc, string? fileName, uint flags)
        {
            var ctx = (SftpContext)fileDesc;
            if ((flags & CleanupDelete) != 0)
            {
                try
                {
                    ctx.Stream?.Close();
                    ctx.Stream = null;
                    _sftp.Delete(ctx.RemotePath);
                }
                catch (Exception ex)
                {
                    // Log but do not propagate: Cleanup must not throw.
                    Console.Error.WriteLine($"[SftpDisk] Cleanup delete failed for '{ctx.RemotePath}': {ex.Message}");
                }
            }
        }

        public override void Close(object fileNode, object fileDesc)
        {
            var ctx = (SftpContext)fileDesc;
            ctx.Stream?.Close();
            ctx.Stream = null;
        }

        public override int Read(
            object fileNode,
            object fileDesc,
            IntPtr buffer,
            ulong offset,
            uint length,
            out uint bytesTransferred)
        {
            bytesTransferred = 0;
            var ctx = (SftpContext)fileDesc;
            if (ctx.Stream == null)
                return STATUS_INVALID_DEVICE_REQUEST;

            try
            {
                ctx.Stream.Position = (long)offset;
                var data = new byte[length];
                int read = ctx.Stream.Read(data, 0, (int)length);
                if (read > 0)
                    Marshal.Copy(data, 0, buffer, read);
                bytesTransferred = (uint)read;
                return STATUS_SUCCESS;
            }
            catch (Exception ex)
            {
                return NtStatusFromException(ex);
            }
        }

        public override int Write(
            object fileNode,
            object fileDesc,
            IntPtr buffer,
            ulong offset,
            uint length,
            bool writeToEndOfFile,
            bool constrainedIo,
            out uint bytesTransferred,
            out FspFileInfo fileInfo)
        {
            bytesTransferred = 0;
            fileInfo         = default;
            var ctx = (SftpContext)fileDesc;
            if (ctx.Stream == null)
                return STATUS_INVALID_DEVICE_REQUEST;

            try
            {
                if (constrainedIo)
                {
                    long fileSize = ctx.Stream.Length;
                    if ((long)offset >= fileSize)
                        return STATUS_SUCCESS;
                    if ((long)(offset + length) > fileSize)
                        length = (uint)(fileSize - (long)offset);
                }

                if (writeToEndOfFile)
                    ctx.Stream.Seek(0, SeekOrigin.End);
                else
                    ctx.Stream.Position = (long)offset;

                var data = new byte[length];
                Marshal.Copy(buffer, data, 0, (int)length);
                ctx.Stream.Write(data, 0, (int)length);
                ctx.Stream.Flush();

                bytesTransferred = length;
                var attrs = _sftp.GetAttributes(ctx.RemotePath);
                fileInfo = MakeFileInfo(attrs.Size, false, attrs.LastAccessTime, attrs.LastWriteTime);
                return STATUS_SUCCESS;
            }
            catch (Exception ex)
            {
                return NtStatusFromException(ex);
            }
        }

        public override int Flush(object fileNode, object fileDesc, out FspFileInfo fileInfo)
        {
            fileInfo = default;
            var ctx = (SftpContext)fileDesc;
            try
            {
                ctx.Stream?.Flush();
                fileInfo = QueryFileInfo(ctx.RemotePath, ctx.IsDirectory);
                return STATUS_SUCCESS;
            }
            catch (Exception ex)
            {
                return NtStatusFromException(ex);
            }
        }

        public override int GetFileInfo(object fileNode, object fileDesc, out FspFileInfo fileInfo)
        {
            fileInfo = default;
            var ctx = (SftpContext)fileDesc;
            try
            {
                fileInfo = QueryFileInfo(ctx.RemotePath, ctx.IsDirectory);
                return STATUS_SUCCESS;
            }
            catch (Exception ex)
            {
                return NtStatusFromException(ex);
            }
        }

        public override int SetBasicInfo(
            object fileNode,
            object fileDesc,
            uint fileAttributes,
            ulong creationTime,
            ulong lastAccessTime,
            ulong lastWriteTime,
            ulong changeTime,
            out FspFileInfo fileInfo)
        {
            fileInfo = default;
            var ctx = (SftpContext)fileDesc;
            try
            {
                var attrs = _sftp.GetAttributes(ctx.RemotePath);
                if (lastAccessTime != 0)
                    attrs.LastAccessTime = DateTime.FromFileTimeUtc((long)lastAccessTime).ToLocalTime();
                if (lastWriteTime != 0)
                    attrs.LastWriteTime = DateTime.FromFileTimeUtc((long)lastWriteTime).ToLocalTime();
                _sftp.SetAttributes(ctx.RemotePath, attrs);
            }
            catch { /* time-stamps are best-effort on SFTP */ }

            try
            {
                fileInfo = QueryFileInfo(ctx.RemotePath, ctx.IsDirectory);
            }
            catch { /* ignore */ }
            return STATUS_SUCCESS;
        }

        public override int SetFileSize(
            object fileNode,
            object fileDesc,
            ulong newSize,
            bool setAllocationSize,
            out FspFileInfo fileInfo)
        {
            fileInfo = default;
            var ctx = (SftpContext)fileDesc;
            if (setAllocationSize)
            {
                // Allocation-size changes are advisory; ignore.
                try { fileInfo = QueryFileInfo(ctx.RemotePath, false); } catch { }
                return STATUS_SUCCESS;
            }
            try
            {
                if (ctx.Stream != null)
                {
                    long currentLen = ctx.Stream.Length;
                    if ((long)newSize < currentLen)
                    {
                        // Truncate: read the content up to newSize, rewrite the file.
                        // SSH.NET SftpFileStream does not expose SetLength.
                        // NOTE: this reads the entire retained portion into memory, which may be
                        // significant for large files. A future improvement is to use SSH_FXP_FSETSTAT
                        // with SSH_FILEXFER_ATTR_SIZE to truncate directly on the server.
                        ctx.Stream.Position = 0;
                        var content = new byte[newSize];
                        int totalRead = 0;
                        while (totalRead < (int)newSize)
                        {
                            int n = ctx.Stream.Read(content, totalRead, (int)newSize - totalRead);
                            if (n == 0) break;
                            totalRead += n;
                        }
                        ctx.Stream.Close();
                        var newStream = _sftp.Open(ctx.RemotePath, FileMode.Create, FileAccess.ReadWrite);
                        newStream.Write(content, 0, totalRead);
                        newStream.Flush();
                        ctx.Stream = newStream;
                    }
                    else if ((long)newSize > currentLen)
                    {
                        // Extend: write zeros to grow the file.
                        ctx.Stream.Seek(0, SeekOrigin.End);
                        long toWrite = (long)newSize - currentLen;
                        var zeros = new byte[Math.Min(toWrite, 65536)];
                        while (toWrite > 0)
                        {
                            int chunk = (int)Math.Min(toWrite, zeros.Length);
                            ctx.Stream.Write(zeros, 0, chunk);
                            toWrite -= chunk;
                        }
                        ctx.Stream.Flush();
                    }
                }
                fileInfo = QueryFileInfo(ctx.RemotePath, false);
                return STATUS_SUCCESS;
            }
            catch (Exception ex)
            {
                return NtStatusFromException(ex);
            }
        }

        public override int CanDelete(object fileNode, object fileDesc, string fileName)
        {
            var ctx = (SftpContext)fileDesc;
            try
            {
                if (ctx.IsDirectory)
                {
                    foreach (var entry in _sftp.ListDirectory(ctx.RemotePath))
                    {
                        if (entry.Name != "." && entry.Name != "..")
                            return STATUS_DIRECTORY_NOT_EMPTY;
                    }
                }
                return STATUS_SUCCESS;
            }
            catch (Exception ex)
            {
                return NtStatusFromException(ex);
            }
        }

        public override int Rename(
            object fileNode,
            object fileDesc,
            string fileName,
            string newFileName,
            bool replaceIfExists)
        {
            try
            {
                var oldPath = ToRemotePath(fileName);
                var newPath = ToRemotePath(newFileName);
                if (replaceIfExists && _sftp.Exists(newPath))
                    _sftp.DeleteFile(newPath);
                _sftp.RenameFile(oldPath, newPath);
                return STATUS_SUCCESS;
            }
            catch (Exception ex)
            {
                return NtStatusFromException(ex);
            }
        }

        public override int GetSecurity(
            object fileNode,
            object fileDesc,
            ref byte[]? securityDescriptor)
        {
            // Return no security descriptor; WinFSP will synthesise a permissive one.
            securityDescriptor = null;
            return STATUS_SUCCESS;
        }

        public override int SetSecurity(
            object fileNode,
            object fileDesc,
            uint securityInformation,
            IntPtr modificationDescriptor)
        {
            // Security attributes are not supported over SFTP; silently ignore.
            return STATUS_SUCCESS;
        }

        // ── Directory enumeration ───────────────────────────────────────────

        /// <summary>Per-enumeration state stored in the <c>context</c> ref parameter.</summary>
        private sealed class DirEnumContext
        {
            public readonly List<(string Name, FspFileInfo Info)> Entries;
            public int Index;

            public DirEnumContext(List<(string, FspFileInfo)> entries)
            {
                Entries = entries;
                Index   = 0;
            }
        }

        public override bool ReadDirectoryEntry(
            object fileNode,
            object fileDesc,
            string? pattern,
            string? marker,
            ref object? context,
            out string? fileName,
            out FspFileInfo fileInfo)
        {
            fileName = null;
            fileInfo = default;
            var ctx  = (SftpContext)fileDesc;

            // Build the entry list on the first call (context == null).
            if (context == null)
            {
                var entries = new List<(string, FspFileInfo)>();
                try
                {
                    foreach (var file in _sftp.ListDirectory(ctx.RemotePath))
                    {
                        var info = MakeFileInfo(
                            file.IsDirectory ? 0L : file.Attributes.Size,
                            file.IsDirectory,
                            file.LastAccessTime,
                            file.LastWriteTime);
                        entries.Add((file.Name, info));
                    }
                }
                catch
                {
                    return false;
                }

                // Sort case-insensitively for consistent Windows Explorer ordering.
                entries.Sort((a, b) =>
                    StringComparer.OrdinalIgnoreCase.Compare(a.Item1, b.Item1));

                var enumCtx = new DirEnumContext(entries);

                // Advance past the marker (return entries strictly after it).
                if (marker != null)
                {
                    enumCtx.Index = entries.Count; // default: marker not found, skip all
                    for (int i = 0; i < entries.Count; i++)
                    {
                        if (StringComparer.OrdinalIgnoreCase.Compare(entries[i].Item1, marker) > 0)
                        {
                            enumCtx.Index = i;
                            break;
                        }
                    }
                }

                context = enumCtx;
            }

            var ec = (DirEnumContext)context;
            if (ec.Index >= ec.Entries.Count)
                return false;

            var entry = ec.Entries[ec.Index++];
            fileName  = entry.Name;
            fileInfo  = entry.Info;
            return true;
        }
    }
}
