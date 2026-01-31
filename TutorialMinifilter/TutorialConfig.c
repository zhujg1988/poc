/*++

Module Name:
    TutorialConfig.c

Abstract:
    教学版透明加解密驱动 - 配置和工具函数
    
    功能：
    1. 判断进程是否授权
    2. 判断文件是否需要监控
    3. 文件扩展名检查

--*/

#include "Tutorial.h"

//
// 配置常量
//

// 授权进程列表（只有这些进程可以读写明文）
// 教学版：只有notepad.exe
CONST PWCHAR AuthorizedProcesses[] = {
    L"notepad.exe",
    NULL  // 结束标记
};

// 监控的文件扩展名
// 教学版：只有.txt
CONST PWCHAR MonitoredExtensions[] = {
    L".txt",
    NULL  // 结束标记
};

// 监控的目录（可选，NULL表示监控所有目录）
// 教学版：为简化，监控所有目录
CONST PWCHAR MonitoredPaths[] = {
    NULL  // NULL表示监控所有路径
};

//
// 工具函数实现
//

BOOLEAN
TutorialIsAuthorizedProcess(
    VOID
)
/*++

功能说明:
    检查当前进程是否为授权进程
    
    实现方法：
    1. 获取当前进程名称
    2. 与授权列表比对（不区分大小写）
    
返回值:
    TRUE - 是授权进程（如notepad.exe）
    FALSE - 非授权进程

--*/
{
    PEPROCESS eProcess = NULL;
    PUCHAR processName = NULL;
    ULONG i;

    //
    // 获取当前进程的EPROCESS结构
    // EPROCESS是内核中表示进程的结构体
    //
    eProcess = PsGetCurrentProcess();
    if (eProcess == NULL) {
        return FALSE;
    }

    //
    // 从EPROCESS结构获取进程名
    // PsGetProcessImageFileName返回进程可执行文件名（不含路径）
    // 例如: "notepad.exe"
    //
    processName = (PUCHAR)PsGetProcessImageFileName(eProcess);
    if (processName == NULL) {
        return FALSE;
    }

    TutorialDbgPrint("IsAuthorizedProcess: 当前进程 = %s\n", processName);

    //
    // 遍历授权进程列表，进行比对
    //
    for (i = 0; AuthorizedProcesses[i] != NULL; i++) {
        
        // 不区分大小写比较
        // _wcsicmp: 宽字符字符串不区分大小写比较
        if (_wcsicmp((PWCHAR)processName, AuthorizedProcesses[i]) == 0) {
            TutorialDbgPrint("IsAuthorizedProcess: 匹配授权进程 %ws\n", 
                AuthorizedProcesses[i]);
            return TRUE;
        }
    }

    TutorialDbgPrint("IsAuthorizedProcess: 非授权进程\n");
    return FALSE;
}

BOOLEAN
TutorialIsTargetExtension(
    _In_ PUNICODE_STRING FileName
)
/*++

功能说明:
    检查文件扩展名是否在监控列表中
    
参数:
    FileName - 文件名（完整路径或相对路径）
    
返回值:
    TRUE - 是目标扩展名（如.txt）
    FALSE - 不是

--*/
{
    PWCHAR extension = NULL;
    ULONG i;
    USHORT nameLen;

    if (FileName == NULL || FileName->Buffer == NULL || FileName->Length == 0) {
        return FALSE;
    }

    nameLen = FileName->Length / sizeof(WCHAR);

    //
    // 从文件名末尾查找扩展名
    // 例如: "C:\test\file.txt" → 找到 ".txt"
    //
    for (i = nameLen - 1; i > 0; i--) {
        if (FileName->Buffer[i] == L'.') {
            extension = &FileName->Buffer[i];
            break;
        }
        // 遇到路径分隔符，说明没有扩展名
        if (FileName->Buffer[i] == L'\\' || FileName->Buffer[i] == L'/') {
            break;
        }
    }

    if (extension == NULL) {
        TutorialDbgPrint("IsTargetExtension: 文件无扩展名\n");
        return FALSE;
    }

    TutorialDbgPrint("IsTargetExtension: 扩展名 = %ws\n", extension);

    //
    // 与监控扩展名列表比对
    //
    for (i = 0; MonitoredExtensions[i] != NULL; i++) {
        if (_wcsicmp(extension, MonitoredExtensions[i]) == 0) {
            TutorialDbgPrint("IsTargetExtension: 匹配监控扩展名 %ws\n", 
                MonitoredExtensions[i]);
            return TRUE;
        }
    }

    return FALSE;
}

BOOLEAN
TutorialIsMonitoredFile(
    _In_ PUNICODE_STRING FileName
)
/*++

功能说明:
    综合判断：文件是否需要监控
    
    判断条件：
    1. 文件扩展名是否匹配（如.txt）
    2. （可选）文件路径是否在监控目录下
    
    教学版简化：只检查扩展名

参数:
    FileName - 文件名
    
返回值:
    TRUE - 需要监控
    FALSE - 不需要监控

--*/
{
    //
    // 教学版简化：只要扩展名匹配就监控
    // 实际产品可能需要：
    // - 检查文件路径是否在指定目录
    // - 检查文件属性（如隐藏、系统文件）
    // - 检查进程权限
    //
    return TutorialIsTargetExtension(FileName);
}
