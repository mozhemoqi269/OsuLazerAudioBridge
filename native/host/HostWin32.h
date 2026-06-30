#pragma once

#include <Windows.h>

#include <filesystem>
#include <optional>
#include <string>

namespace olab::host {

struct UniqueHandle {
    HANDLE handle = nullptr;

    UniqueHandle() = default;
    explicit UniqueHandle(HANDLE value);
    UniqueHandle(const UniqueHandle&) = delete;
    UniqueHandle& operator=(const UniqueHandle&) = delete;
    UniqueHandle(UniqueHandle&& other) noexcept;
    UniqueHandle& operator=(UniqueHandle&& other) noexcept;
    ~UniqueHandle();

    void Reset(HANDLE value = nullptr);
    HANDLE Get() const;
    explicit operator bool() const;
};

std::optional<DWORD> FindProcessId(const std::wstring& processName);
std::filesystem::path CurrentExeDirectory();
std::wstring QuoteCommandLineArgument(const std::wstring& value);
bool InjectDll(DWORD processId, const std::filesystem::path& dllPath);

} // namespace olab::host
