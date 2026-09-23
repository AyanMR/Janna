#include "services/GamePathResolver.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcessEnvironment>
#include <QSettings>

#include <Windows.h>
#include <TlHelp32.h>
#include <algorithm>
#include <string>

namespace Janna {
namespace {
constexpr auto manualEnabledKey = "lcu/manualGamePathEnabled";
constexpr auto manualPathKey = "lcu/manualGamePath";

void appendUnique(QStringList &paths, const QString &path)
{
    if (path.isEmpty()) return;
    if (std::none_of(paths.cbegin(), paths.cend(), [&path](const QString &existing) {
            return existing.compare(path, Qt::CaseInsensitive) == 0;
        })) {
        paths.append(path);
    }
}

QString normalizedLeagueClientPath(const QString &path)
{
    if (path.isEmpty()) return {};
    const QFileInfo supplied(path);
    const QString executable = supplied.isDir()
        ? QDir(supplied.absoluteFilePath()).filePath("LeagueClient.exe")
        : supplied.absoluteFilePath();
    const QFileInfo candidate(executable);
    if (!candidate.isFile() || candidate.fileName().compare("LeagueClient.exe", Qt::CaseInsensitive) != 0) return {};
    return QDir::cleanPath(candidate.absoluteFilePath());
}

QString executablePathForProcess(const DWORD processId)
{
    const HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, processId);
    if (!process) return {};

    std::wstring buffer(32768, L'\0');
    DWORD size = static_cast<DWORD>(buffer.size());
    const bool resolved = QueryFullProcessImageNameW(process, 0, buffer.data(), &size);
    CloseHandle(process);
    if (!resolved || size == 0) return {};
    return QString::fromWCharArray(buffer.data(), static_cast<int>(size));
}

QString runningLeagueClientPath()
{
    const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return {};

    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    QString result;
    if (Process32FirstW(snapshot, &entry)) {
        do {
            if (_wcsicmp(entry.szExeFile, L"LeagueClient.exe") == 0) {
                result = normalizedLeagueClientPath(executablePathForProcess(entry.th32ProcessID));
                if (!result.isEmpty()) break;
            }
        } while (Process32NextW(snapshot, &entry));
    }
    CloseHandle(snapshot);
    return result;
}

QStringList metadataLeagueClientPaths()
{
    const QString programData = qEnvironmentVariable("ProgramData", "C:/ProgramData");
    QFile metadata(QDir(programData).filePath("Riot Games/RiotClientInstalls.json"));
    if (!metadata.open(QIODevice::ReadOnly)) return {};

    const QJsonDocument document = QJsonDocument::fromJson(metadata.readAll());
    const QJsonObject associatedClients = document.object().value("associated_client").toObject();
    QStringList paths;
    for (auto it = associatedClients.constBegin(); it != associatedClients.constEnd(); ++it) {
        appendUnique(paths, normalizedLeagueClientPath(QDir::cleanPath(QDir::fromNativeSeparators(it.key()))));
    }
    return paths;
}

QStringList knownLeagueClientPaths()
{
    QStringList paths = metadataLeagueClientPaths();
    appendUnique(paths, normalizedLeagueClientPath("C:/Riot Games/League of Legends/LeagueClient.exe"));
    appendUnique(paths, normalizedLeagueClientPath("D:/Riot Games/League of Legends/LeagueClient.exe"));
    appendUnique(paths, normalizedLeagueClientPath(QDir::home().filePath("AppData/Local/Riot Games/League of Legends/LeagueClient.exe")));
    return paths;
}

} // namespace

QString GamePathResolver::configuredLeagueClientPath()
{
    QSettings settings;
    return QDir::cleanPath(settings.value(manualPathKey).toString());
}

bool GamePathResolver::usesManualPath()
{
    QSettings settings;
    return settings.value(manualEnabledKey, false).toBool();
}

bool GamePathResolver::setManualLeagueClientPath(const QString &path)
{
    const QString executable = normalizedLeagueClientPath(path);
    if (executable.isEmpty()) return false;

    QSettings settings;
    settings.setValue(manualPathKey, executable);
    settings.setValue(manualEnabledKey, true);
    return true;
}

void GamePathResolver::useAutomaticPath()
{
    QSettings settings;
    settings.setValue(manualEnabledKey, false);
}

QString GamePathResolver::detectedLeagueClientPath()
{
    const QString running = runningLeagueClientPath();
    if (!running.isEmpty()) return running;

    const QStringList paths = knownLeagueClientPaths();
    return paths.isEmpty() ? QString() : paths.constFirst();
}

QString GamePathResolver::leagueClientPath()
{
    if (usesManualPath()) {
        const QString configured = normalizedLeagueClientPath(configuredLeagueClientPath());
        if (!configured.isEmpty()) return configured;
    }
    return detectedLeagueClientPath();
}

QString GamePathResolver::gameDirectory()
{
    const QString executable = leagueClientPath();
    return executable.isEmpty() ? QString() : QFileInfo(executable).absolutePath();
}

QStringList GamePathResolver::lockfileCandidates()
{
    QStringList candidates;
    const QString configuredLockfile = QProcessEnvironment::systemEnvironment().value("LCU_LOCKFILE");
    appendUnique(candidates, configuredLockfile);

    const QString executable = leagueClientPath();
    if (!executable.isEmpty()) appendUnique(candidates, QDir(QFileInfo(executable).absolutePath()).filePath("lockfile"));
    for (const QString &knownPath : knownLeagueClientPaths()) {
        appendUnique(candidates, QDir(QFileInfo(knownPath).absolutePath()).filePath("lockfile"));
    }
    return candidates;
}

} // namespace Janna
