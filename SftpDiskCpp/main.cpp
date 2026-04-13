// main.cpp — SftpDiskCpp entry point.
//
// Mounts a remote SFTP share as a local Windows drive letter using WinFSP (user-mode
// file-system driver) and libssh2 (SSH/SFTP client library).
//
// Usage:
//   SftpDiskCpp.exe --host <host> [--port <port>] --user <user>
//                   (--password <pass> | --key <pem-file>)
//                   --mount <X:> [--path <remote-dir>]
//
// Prerequisites:
//   * WinFSP installed (https://winfsp.dev)
//   * Run as Administrator (required to mount a drive letter)

// winsock2 must be included before windows.h to avoid the old winsock.h being
// pulled in transitively.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>

#include <libssh2.h>
#include <libssh2_sftp.h>

#include "SftpFileSystem.h"  // pulls in winfsp.hpp transitively

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#pragma comment(lib, "ws2_32.lib")

// ── global stop event (signalled by Ctrl+C handler) ─────────────────────────

static HANDLE g_stopEvent = nullptr;

static BOOL WINAPI ConsoleCtrlHandler(DWORD ctrlType)
{
    switch (ctrlType) {
    case CTRL_C_EVENT:
    case CTRL_BREAK_EVENT:
    case CTRL_CLOSE_EVENT:
        if (g_stopEvent)
            SetEvent(g_stopEvent);
        return TRUE;
    default:
        return FALSE;
    }
}

// ── usage ────────────────────────────────────────────────────────────────────

static void PrintUsage(const char *prog)
{
    fprintf(stderr,
        "\nSftpDiskCpp -- mount a remote SFTP share as a local Windows drive\n"
        "               (WinFSP + libssh2)\n\n"
        "Usage:\n"
        "  %s --host <host> [--port <port>] --user <user>\n"
        "     (--password <pass> | --key <pem-file>)\n"
        "     --mount <X:> [--path <remote-dir>]\n\n"
        "Options:\n"
        "  --host <host>       SFTP server hostname or IP address\n"
        "  --port <port>       SSH port (default: 22)\n"
        "  --user <user>       SSH username\n"
        "  --password <pass>   Authenticate with password\n"
        "  --key <pem-file>    Authenticate with PEM private key file\n"
        "  --mount <X:>        Local drive letter to mount (e.g. Z:)\n"
        "  --path <dir>        Remote root directory (default: /)\n\n",
        prog);
}

// ── main ─────────────────────────────────────────────────────────────────────

int main(int argc, char *argv[])
{
    // ── argument parsing ─────────────────────────────────────────────────────

    const char *host       = nullptr;
    int         port       = 22;
    const char *user       = nullptr;
    const char *password   = nullptr;
    const char *keyFile    = nullptr;
    const char *mountPoint = nullptr;
    const char *remotePath = "/";

    for (int i = 1; i < argc; ++i) {
        if      (!strcmp(argv[i], "--host")     && i+1 < argc) host       = argv[++i];
        else if (!strcmp(argv[i], "--port")     && i+1 < argc) port       = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--user")     && i+1 < argc) user       = argv[++i];
        else if (!strcmp(argv[i], "--password") && i+1 < argc) password   = argv[++i];
        else if (!strcmp(argv[i], "--key")      && i+1 < argc) keyFile    = argv[++i];
        else if (!strcmp(argv[i], "--mount")    && i+1 < argc) mountPoint = argv[++i];
        else if (!strcmp(argv[i], "--path")     && i+1 < argc) remotePath = argv[++i];
        else {
            fprintf(stderr, "ERROR: Unknown argument: %s\n", argv[i]);
            PrintUsage(argv[0]);
            return 1;
        }
    }

    if (!host || !user || !mountPoint) {
        fprintf(stderr, "ERROR: --host, --user, and --mount are required.\n");
        PrintUsage(argv[0]);
        return 1;
    }
    if (!password && !keyFile) {
        fprintf(stderr, "ERROR: Either --password or --key must be specified.\n");
        PrintUsage(argv[0]);
        return 1;
    }

    // ── Winsock initialisation ───────────────────────────────────────────────

    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        fprintf(stderr, "ERROR: WSAStartup failed.\n");
        return 1;
    }

    // ── libssh2 global initialisation ────────────────────────────────────────

    if (libssh2_init(0) != 0) {
        fprintf(stderr, "ERROR: libssh2_init failed.\n");
        WSACleanup();
        return 1;
    }

    // ── resource handles (all released in the cleanup block) ─────────────────

    SOCKET            sock    = INVALID_SOCKET;
    LIBSSH2_SESSION  *session = nullptr;
    LIBSSH2_SFTP     *sftp   = nullptr;
    SftpFileSystem   *fs     = nullptr;
    Fsp::FileSystemHost *host_obj = nullptr;
    int exitCode = 1;

    // ── resolve and connect ──────────────────────────────────────────────────

    {
        char portStr[8];
        snprintf(portStr, sizeof(portStr), "%d", port);

        printf("[*] Resolving %s:%d ...\n", host, port);

        struct addrinfo hints{}, *res = nullptr;
        hints.ai_family   = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;

        if (getaddrinfo(host, portStr, &hints, &res) != 0) {
            fprintf(stderr, "ERROR: Failed to resolve host '%s'.\n", host);
            goto cleanup;
        }

        sock = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
        if (sock == INVALID_SOCKET) {
            fprintf(stderr, "ERROR: socket() failed.\n");
            freeaddrinfo(res);
            goto cleanup;
        }

        if (connect(sock, res->ai_addr, static_cast<int>(res->ai_addrlen)) != 0) {
            fprintf(stderr, "ERROR: connect() to %s:%d failed.\n", host, port);
            freeaddrinfo(res);
            goto cleanup;
        }
        freeaddrinfo(res);
        printf("[*] TCP connected.\n");
    }

    // ── SSH handshake ────────────────────────────────────────────────────────

    session = libssh2_session_init();
    if (!session) {
        fprintf(stderr, "ERROR: libssh2_session_init failed.\n");
        goto cleanup;
    }
    libssh2_session_set_blocking(session, 1);

    if (libssh2_session_handshake(session, static_cast<libssh2_socket_t>(sock)) != 0) {
        char *msg = nullptr;
        libssh2_session_last_error(session, &msg, nullptr, 0);
        fprintf(stderr, "ERROR: SSH handshake failed: %s\n", msg ? msg : "unknown");
        goto cleanup;
    }
    printf("[*] SSH handshake complete.\n");

    // ── authentication ───────────────────────────────────────────────────────

    if (keyFile) {
        printf("[*] Authenticating with private key '%s' ...\n", keyFile);
        if (libssh2_userauth_publickey_fromfile(
                session, user,
                nullptr,  // public key file (derived from private key when nullptr)
                keyFile,
                nullptr   // passphrase (nullptr = no passphrase)
            ) != 0)
        {
            char *msg = nullptr;
            libssh2_session_last_error(session, &msg, nullptr, 0);
            fprintf(stderr, "ERROR: Key authentication failed: %s\n", msg ? msg : "unknown");
            goto cleanup;
        }
    } else {
        printf("[*] Authenticating with password ...\n");
        if (libssh2_userauth_password(session, user, password) != 0) {
            char *msg = nullptr;
            libssh2_session_last_error(session, &msg, nullptr, 0);
            fprintf(stderr, "ERROR: Password authentication failed: %s\n", msg ? msg : "unknown");
            goto cleanup;
        }
    }
    printf("[*] Authenticated as '%s'.\n", user);

    // ── SFTP subsystem ───────────────────────────────────────────────────────

    sftp = libssh2_sftp_init(session);
    if (!sftp) {
        fprintf(stderr, "ERROR: Failed to initialise SFTP subsystem.\n");
        goto cleanup;
    }
    printf("[*] SFTP subsystem ready.\n");

    // ── WinFSP filesystem ────────────────────────────────────────────────────

    {
        int wlen = MultiByteToWideChar(CP_UTF8, 0, mountPoint, -1, nullptr, 0);
        std::wstring mountPointW(wlen - 1, L'\0');
        MultiByteToWideChar(CP_UTF8, 0, mountPoint, -1, &mountPointW[0], wlen);

        fs       = new SftpFileSystem(sftp, remotePath);
        host_obj = new Fsp::FileSystemHost(*fs);

        host_obj->SetFileSystemName(L"SFTP");
        host_obj->SetSectorSize(512);
        host_obj->SetSectorsPerAllocationUnit(8);    // 4 KiB cluster
        host_obj->SetMaxComponentLength(255);
        host_obj->SetCaseSensitiveSearch(TRUE);
        host_obj->SetCasePreservedNames(TRUE);
        host_obj->SetUnicodeOnDisk(TRUE);
        host_obj->SetPersistentAcls(FALSE);
        host_obj->SetPassQueryDirectoryPattern(FALSE); // WinFSP handles wildcard filtering

        FILETIME ft;
        GetSystemTimeAsFileTime(&ft);
        UINT64 now = (static_cast<UINT64>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
        host_obj->SetVolumeCreationTime(now);
        host_obj->SetVolumeSerialNumber(0x53465450); // 'SFTP'

        printf("[*] Mounting at %s (remote root: %s) ...\n", mountPoint, remotePath);

        NTSTATUS status = host_obj->Mount(mountPointW.c_str(),
                                          nullptr, // security descriptor
                                          TRUE,    // synchronised (safer for multi-thread)
                                          0);      // debug log flags
        if (!NT_SUCCESS(status)) {
            fprintf(stderr,
                "ERROR: Mount failed (NTSTATUS 0x%08X).\n"
                "       Ensure WinFSP is installed and you are running as Administrator.\n",
                static_cast<unsigned>(status));
            goto cleanup;
        }

        printf("[+] Drive mounted at %s\n", mountPoint);
        printf("    Press Ctrl+C to unmount and exit.\n");

        // ── wait for stop signal ─────────────────────────────────────────────

        g_stopEvent = CreateEvent(nullptr, TRUE, FALSE, nullptr);
        SetConsoleCtrlHandler(ConsoleCtrlHandler, TRUE);
        WaitForSingleObject(g_stopEvent, INFINITE);
        SetConsoleCtrlHandler(ConsoleCtrlHandler, FALSE);
        CloseHandle(g_stopEvent);
        g_stopEvent = nullptr;

        printf("[*] Unmounting ...\n");
        host_obj->Unmount();
        printf("[*] Done.\n");
    }

    exitCode = 0;

cleanup:
    delete host_obj;
    delete fs;

    if (sftp)    libssh2_sftp_shutdown(sftp);
    if (session) {
        libssh2_session_disconnect(session, "Goodbye");
        libssh2_session_free(session);
    }
    if (sock != INVALID_SOCKET)
        closesocket(sock);

    libssh2_exit();
    WSACleanup();

    return exitCode;
}
