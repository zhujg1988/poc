using System;
using System.Threading;
using Fsp;
using Renci.SshNet;

namespace SftpDisk
{
    /// <summary>
    /// Entry point for the SftpDisk utility.
    ///
    /// Usage:
    ///   SftpDisk.exe --host &lt;host&gt; [--port &lt;port&gt;] --user &lt;user&gt;
    ///               (--password &lt;password&gt; | --key &lt;private-key-file&gt;)
    ///               --mount &lt;drive-letter&gt; [--path &lt;remote-root&gt;]
    ///
    /// Examples:
    ///   SftpDisk.exe --host 192.168.1.10 --user alice --password s3cr3t --mount Z:
    ///   SftpDisk.exe --host myserver.local --user bob --key C:\Users\bob\.ssh\id_rsa --mount Y: --path /data
    ///
    /// Prerequisites:
    ///   * WinFSP must be installed (https://winfsp.dev).
    ///   * The process must run with sufficient privilege to create a virtual disk.
    ///   * The target drive letter must not already be in use.
    /// </summary>
    internal static class Program
    {
        private static int Main(string[] args)
        {
            // ── argument parsing ────────────────────────────────────────────
            string? host       = null;
            int     port       = 22;
            string? user       = null;
            string? password   = null;
            string? keyFile    = null;
            string? mountPoint = null;
            string  remotePath = "/";

            for (int i = 0; i < args.Length; i++)
            {
                switch (args[i])
                {
                    case "--host":     host       = args[++i]; break;
                    case "--port":     port       = int.Parse(args[++i]); break;
                    case "--user":     user       = args[++i]; break;
                    case "--password": password   = args[++i]; break;
                    case "--key":      keyFile    = args[++i]; break;
                    case "--mount":    mountPoint = args[++i]; break;
                    case "--path":     remotePath = args[++i]; break;
                    default:
                        Console.Error.WriteLine($"Unknown argument: {args[i]}");
                        return PrintUsage();
                }
            }

            if (host == null || user == null || mountPoint == null)
                return PrintUsage();

            if (password == null && keyFile == null)
            {
                Console.Error.WriteLine("ERROR: Either --password or --key must be supplied.");
                return PrintUsage();
            }

            // ── SFTP connection ─────────────────────────────────────────────
            AuthenticationMethod auth = keyFile != null
                ? (AuthenticationMethod)new PrivateKeyAuthenticationMethod(user, new PrivateKeyFile(keyFile))
                : new PasswordAuthenticationMethod(user, password!);

            var connInfo   = new ConnectionInfo(host, port, user, auth);
            var sftpClient = new SftpClient(connInfo);

            try
            {
                Console.WriteLine($"Connecting to {host}:{port} as {user} …");
                sftpClient.Connect();
                Console.WriteLine("Connected.");

                // ── WinFSP host setup ───────────────────────────────────────
                var fileSystem = new SftpFileSystem(sftpClient, remotePath);
                var fspHost    = new FileSystemHost(fileSystem)
                {
                    FileSystemName           = "SFTP",
                    SectorSize               = 512,
                    SectorsPerAllocationUnit = 8,          // 4 KiB cluster
                    MaxComponentLength       = 255,
                    CaseSensitiveSearch      = true,       // SFTP servers are usually case-sensitive
                    CasePreservedNames       = true,
                    UnicodeOnDisk            = true,
                    PersistentAcls           = false,
                    PassQueryDirectoryPattern = false,     // let WinFSP filter wildcard patterns
                    VolumeCreationTime       = (ulong)DateTime.UtcNow.ToFileTimeUtc(),
                    VolumeSerialNumber       = (uint)(host.GetHashCode() ^ remotePath.GetHashCode()),
                };

                int mountResult = fspHost.Mount(mountPoint, null, true, 0);
                if (mountResult != 0)
                {
                    Console.Error.WriteLine($"ERROR: Failed to mount file system (NTSTATUS 0x{mountResult:X8}).");
                    Console.Error.WriteLine("Ensure WinFSP is installed and you have administrator privileges.");
                    return 1;
                }

                Console.WriteLine($"SFTP drive mounted at {mountPoint}  (remote root: {remotePath})");
                Console.WriteLine("Press Ctrl+C to unmount and exit.");

                // ── wait for Ctrl+C ─────────────────────────────────────────
                using var cts = new CancellationTokenSource();
                Console.CancelKeyPress += (_, e) =>
                {
                    e.Cancel = true;
                    cts.Cancel();
                };

                cts.Token.WaitHandle.WaitOne();

                Console.WriteLine("Unmounting …");
                fspHost.Unmount();
                Console.WriteLine("Done.");
            }
            catch (Exception ex)
            {
                Console.Error.WriteLine($"ERROR: {ex.Message}");
                return 1;
            }
            finally
            {
                sftpClient.Disconnect();
                sftpClient.Dispose();
            }

            return 0;
        }

        private static int PrintUsage()
        {
            Console.Error.WriteLine();
            Console.Error.WriteLine("SftpDisk — mount a remote SFTP share as a local Windows drive using WinFSP");
            Console.Error.WriteLine();
            Console.Error.WriteLine("Usage:");
            Console.Error.WriteLine("  SftpDisk.exe --host <host> [--port <port>] --user <user>");
            Console.Error.WriteLine("               (--password <pass> | --key <key-file>)");
            Console.Error.WriteLine("               --mount <X:> [--path <remote-path>]");
            Console.Error.WriteLine();
            Console.Error.WriteLine("Options:");
            Console.Error.WriteLine("  --host <host>        SFTP server hostname or IP address");
            Console.Error.WriteLine("  --port <port>        SFTP port (default: 22)");
            Console.Error.WriteLine("  --user <user>        SSH username");
            Console.Error.WriteLine("  --password <pass>    Password authentication");
            Console.Error.WriteLine("  --key <key-file>     Path to a PEM private-key file for key authentication");
            Console.Error.WriteLine("  --mount <X:>         Local drive letter to mount on (e.g. Z:)");
            Console.Error.WriteLine("  --path <path>        Remote root directory (default: /)");
            Console.Error.WriteLine();
            return 1;
        }
    }
}
