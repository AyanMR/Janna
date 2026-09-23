#include "services/LcuCredentialProvider.h"

#include "services/GamePathResolver.h"

#include <QFile>
#include <QFileInfo>
#include <QStringList>

#include <Windows.h>
#include <Shellapi.h>
#include <TlHelp32.h>

#include <cstddef>
#include <vector>

namespace Janna {
namespace {

constexpr ULONG processCommandLineInformation = 60;
constexpr ULONG maximumCommandLineBytes = 1024 * 1024;
constexpr LONG statusAccessDenied = static_cast<LONG>(0xC0000022UL);

using NtQueryInformationProcessFunction = LONG(NTAPI *)(HANDLE, ULONG, PVOID, ULONG, PULONG);

struct NativeUnicodeString {
    USHORT length;
    USHORT maximumLength;
    PWSTR buffer;
};

struct CommandLineLookup {
    QString commandLine;
    bool accessDenied{};
};

struct RuntimeLookup {
    LcuCredentials credentials;
    bool clientFound{};
    bool accessDenied{};
};

struct LockfileLookup {
    LcuCredentials credentials;
    bool found{};
    bool empty{};
    bool invalid{};
};

bool isValidPort(const QString &port)
{
    bool converted = false;
    const ushort number = port.toUShort(&converted);
    return converted && number != 0;
}

CommandLineLookup readProcessCommandLine(const DWORD processId)
{
    CommandLineLookup lookup;
    const HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, processId);
    if (!process) {
        lookup.accessDenied = GetLastError() == ERROR_ACCESS_DENIED;
        return lookup;
    }

    const HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    const auto queryInformation = ntdll == nullptr
        ? nullptr
        : reinterpret_cast<NtQueryInformationProcessFunction>(GetProcAddress(ntdll, "NtQueryInformationProcess"));
    if (!queryInformation) {
        CloseHandle(process);
        return lookup;
    }

    ULONG requiredBytes = 0;
    const LONG sizingStatus = queryInformation(process, processCommandLineInformation, nullptr, 0, &requiredBytes);
    if (requiredBytes == 0 || requiredBytes > maximumCommandLineBytes) {
        lookup.accessDenied = sizingStatus == statusAccessDenied;
        CloseHandle(process);
        return lookup;
    }

    const size_t itemCount = (static_cast<size_t>(requiredBytes) + sizeof(std::max_align_t) - 1) / sizeof(std::max_align_t);
    std::vector<std::max_align_t> storage(itemCount);
    ULONG returnedBytes = 0;
    const LONG queryStatus = queryInformation(process, processCommandLineInformation, storage.data(), requiredBytes, &returnedBytes);
    CloseHandle(process);
    if (queryStatus < 0) {
        lookup.accessDenied = queryStatus == statusAccessDenied;
        return lookup;
    }

    const auto *value = reinterpret_cast<const NativeUnicodeString *>(storage.data());
    if (!value->buffer || value->length == 0 || value->length % sizeof(wchar_t) != 0) return lookup;
    lookup.commandLine = QString::fromWCharArray(value->buffer, static_cast<int>(value->length / sizeof(wchar_t)));
    return lookup;
}

QStringList commandLineArguments(const QString &commandLine)
{
    int argumentCount = 0;
    LPWSTR *arguments = CommandLineToArgvW(reinterpret_cast<LPCWSTR>(commandLine.utf16()), &argumentCount);
    if (!arguments) return {};

    QStringList result;
    result.reserve(argumentCount);
    for (int index = 0; index < argumentCount; ++index) result.append(QString::fromWCharArray(arguments[index]));
    LocalFree(arguments);
    return result;
}

QString argumentValue(const QStringList &arguments, const QString &name)
{
    const QString flag = "--" + name;
    const QString equalsFlag = flag + '=';
    for (int index = 0; index < arguments.size(); ++index) {
        const QString &argument = arguments[index];
        if (argument.compare(flag, Qt::CaseInsensitive) == 0 && index + 1 < arguments.size()) return arguments[index + 1].trimmed();
        if (argument.startsWith(equalsFlag, Qt::CaseInsensitive)) return argument.mid(equalsFlag.size()).trimmed();
    }
    return {};
}

RuntimeLookup readRuntimeCredentials()
{
    RuntimeLookup lookup;
    const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return lookup;

    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    if (Process32FirstW(snapshot, &entry)) {
        do {
            if (_wcsicmp(entry.szExeFile, L"LeagueClientUx.exe") != 0) continue;
            lookup.clientFound = true;

            const CommandLineLookup commandLine = readProcessCommandLine(entry.th32ProcessID);
            lookup.accessDenied = lookup.accessDenied || commandLine.accessDenied;
            if (commandLine.commandLine.isEmpty()) continue;

            const QStringList arguments = commandLineArguments(commandLine.commandLine);
            const QString port = argumentValue(arguments, "app-port");
            const QString token = argumentValue(arguments, "remoting-auth-token");
            if (!isValidPort(port) || token.isEmpty()) continue;

            lookup.credentials = {port, token};
            break;
        } while (Process32NextW(snapshot, &entry));
    }
    CloseHandle(snapshot);
    return lookup;
}

LockfileLookup readLockfileCredentials()
{
    LockfileLookup lookup;
    for (const QString &path : GamePathResolver::lockfileCandidates()) {
        const QFileInfo info(path);
        if (!info.exists()) continue;
        lookup.found = true;

        QFile file(path);
        if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
            lookup.invalid = true;
            continue;
        }

        const QString contents = QString::fromUtf8(file.readAll()).trimmed();
        if (contents.isEmpty()) {
            lookup.empty = true;
            continue;
        }

        const QStringList fields = contents.split(':');
        if (fields.size() != 5 || !isValidPort(fields[2]) || fields[3].isEmpty()) {
            lookup.invalid = true;
            continue;
        }

        lookup.credentials = {fields[2], fields[3]};
        break;
    }
    return lookup;
}

} // namespace

LcuCredentials LcuCredentialProvider::discover(QString &error)
{
    error.clear();
    const LockfileLookup lockfile = readLockfileCredentials();
    if (lockfile.credentials.isValid()) return lockfile.credentials;

    // Tencent's client leaves the legacy lockfile empty and supplies these values to LeagueClientUx instead.
    const RuntimeLookup runtime = readRuntimeCredentials();
    if (runtime.credentials.isValid()) return runtime.credentials;

    if (lockfile.empty) {
        error = runtime.accessDenied
            ? "已检测到 LeagueClient.exe，但传统 LCU lockfile 为空，且当前没有权限读取 LeagueClientUx.exe 的本地 LCU 参数。"
            : "已检测到 LeagueClient.exe，但传统 LCU lockfile 为空，LeagueClientUx.exe 未提供可用的本地 LCU 参数。";
    } else if (lockfile.invalid || lockfile.found) {
        error = "已检测到 LCU lockfile，但其格式无效，且未找到可用的运行时 LCU 参数。";
    } else if (runtime.clientFound) {
        error = runtime.accessDenied
            ? "已检测到 LeagueClientUx.exe，但当前没有权限读取它的本地 LCU 参数。"
            : "已检测到 LeagueClientUx.exe，但未找到可用的本地 LCU 参数。";
    } else if (!GamePathResolver::leagueClientPath().isEmpty()) {
        error = "已检测到游戏目录，但未找到可用的 LCU lockfile 或运行时 LCU 参数。";
    } else {
        error = "未找到 LeagueClient.exe。请在设置中自动检测或手动选择游戏目录。";
    }
    return {};
}

} // namespace Janna
