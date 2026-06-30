#include "HostLogging.h"

#include <Windows.h>

#include <chrono>
#include <ctime>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>

namespace olab::host {
namespace {

std::wstring FormatLocalTimestampForFile()
{
    const auto now = std::chrono::system_clock::now();
    const std::time_t time = std::chrono::system_clock::to_time_t(now);
    std::tm local {};
    localtime_s(&local, &time);

    std::wstringstream stream;
    stream
        << std::put_time(&local, L"%Y%m%d-%H%M%S");
    return stream.str();
}

std::string WideToUtf8(const std::wstring& value)
{
    if (value.empty())
        return {};

    int length = WideCharToMultiByte(
        CP_UTF8,
        0,
        value.c_str(),
        static_cast<int>(value.size()),
        nullptr,
        0,
        nullptr,
        nullptr);
    if (length <= 0)
        return {};

    std::string result(static_cast<std::size_t>(length), '\0');
    WideCharToMultiByte(
        CP_UTF8,
        0,
        value.c_str(),
        static_cast<int>(value.size()),
        result.data(),
        length,
        nullptr,
        nullptr);
    return result;
}

} // namespace

ProbeLog::ProbeLog(std::optional<std::filesystem::path> path)
{
    if (!path)
        return;

    std::filesystem::create_directories(path->parent_path());
    stream.open(*path, std::ios::binary | std::ios::out | std::ios::trunc);
    if (!stream)
        throw std::runtime_error("Failed to open probe log file.");

    logPath = std::filesystem::absolute(*path);
    static constexpr unsigned char utf8Bom[] = { 0xEF, 0xBB, 0xBF };
    stream.write(reinterpret_cast<const char*>(utf8Bom), sizeof(utf8Bom));
}

bool ProbeLog::IsEnabled() const
{
    return stream.is_open();
}

const std::filesystem::path& ProbeLog::Path() const
{
    return logPath;
}

void ProbeLog::WriteLine(const std::wstring& line)
{
    if (!stream.is_open())
        return;

    const std::string utf8 = WideToUtf8(line);
    stream.write(utf8.data(), static_cast<std::streamsize>(utf8.size()));
    stream.put('\n');
}

std::filesystem::path DefaultLogPath(const std::filesystem::path& logDirectory)
{
    return logDirectory / (L"probe-" + FormatLocalTimestampForFile() + L".log");
}

void PrintAndLogLine(const std::wstring& line, ProbeLog& log)
{
    std::wcout << line << L"\n";
    log.WriteLine(line);
}

} // namespace olab::host
