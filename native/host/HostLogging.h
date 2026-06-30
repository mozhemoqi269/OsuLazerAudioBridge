#pragma once

#include <filesystem>
#include <fstream>
#include <optional>
#include <string>

namespace olab::host {

class ProbeLog {
public:
    explicit ProbeLog(std::optional<std::filesystem::path> path);

    bool IsEnabled() const;
    const std::filesystem::path& Path() const;
    void WriteLine(const std::wstring& line);

private:
    std::filesystem::path logPath;
    std::ofstream stream;
};

std::filesystem::path DefaultLogPath(const std::filesystem::path& logDirectory);
void PrintAndLogLine(const std::wstring& line, ProbeLog& log);

} // namespace olab::host
