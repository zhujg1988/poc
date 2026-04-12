using Renci.SshNet.Sftp;

namespace SftpDisk
{
    /// <summary>
    /// Holds state for an open file or directory handle returned to WinFSP.
    /// </summary>
    internal sealed class SftpContext
    {
        /// <summary>Absolute path on the remote SFTP server.</summary>
        public string RemotePath { get; }

        /// <summary>True when the handle refers to a directory.</summary>
        public bool IsDirectory { get; }

        /// <summary>
        /// Open stream for a regular file; null for directories.
        /// May be replaced when the file is truncated (Overwrite / SetFileSize).
        /// </summary>
        public SftpFileStream? Stream { get; set; }

        public SftpContext(string remotePath, bool isDirectory, SftpFileStream? stream)
        {
            RemotePath = remotePath;
            IsDirectory = isDirectory;
            Stream = stream;
        }
    }
}
