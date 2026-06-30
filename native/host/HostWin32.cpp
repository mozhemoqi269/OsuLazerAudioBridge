#include "HostWin32.h"

#include <TlHelp32.h>

#include <iostream>

namespace olab::host {

UniqueHandle::UniqueHandle(HANDLE value)
    : handle(value)
{
}

UniqueHandle::UniqueHandle(UniqueHandle&& other) noexcept
    : handle(other.handle)
{
    other.handle = nullptr;
}

UniqueHandle& UniqueHandle::operator=(UniqueHandle&& other) noexcept
{
    if (this == &other)
        return *this;

    Reset();
    handle = other.handle;
    other.handle = nullptr;
    return *this;
}

UniqueHandle::~UniqueHandle()
{
    Reset();
}

void UniqueHandle::Reset(HANDLE value)
{
    if (handle != nullptr)
        CloseHandle(handle);
    handle = value;
}

HANDLE UniqueHandle::Get() const
{
    return handle;
}

UniqueHandle::operator bool() const
{
    return handle != nullptr;
}

std::optional<DWORD> FindProcessId(const std::wstring& processName)
{
    HANDLE rawSnapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (rawSnapshot == INVALID_HANDLE_VALUE)
        return std::nullopt;
    UniqueHandle snapshot(rawSnapshot);

    PROCESSENTRY32W entry {};
    entry.dwSize = sizeof(entry);
    if (!Process32FirstW(snapshot.Get(), &entry))
        return std::nullopt;

    do {
        if (_wcsicmp(entry.szExeFile, processName.c_str()) == 0)
            return entry.th32ProcessID;
    } while (Process32NextW(snapshot.Get(), &entry));

    return std::nullopt;
}

std::filesystem::path CurrentExeDirectory()
{
    std::wstring buffer(MAX_PATH, L'\0');
    DWORD length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    while (length == buffer.size()) {
        buffer.resize(buffer.size() * 2);
        length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    }

    buffer.resize(length);
    return std::filesystem::path(buffer).parent_path();
}

std::wstring QuoteCommandLineArgument(const std::wstring& value)
{
    std::wstring result = L"\"";
    unsigned int backslashes = 0;
    for (wchar_t ch : value) {
        if (ch == L'\\') {
            backslashes++;
            continue;
        }

        if (ch == L'"') {
            result.append(backslashes * 2 + 1, L'\\');
            result.push_back(ch);
        } else {
            result.append(backslashes, L'\\');
            result.push_back(ch);
        }
        backslashes = 0;
    }

    result.append(backslashes * 2, L'\\');
    result.push_back(L'"');
    return result;
}

bool InjectDll(DWORD processId, const std::filesystem::path& dllPath)
{
    const std::wstring path = std::filesystem::absolute(dllPath).wstring();
    UniqueHandle process(OpenProcess(
        PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION | PROCESS_VM_OPERATION | PROCESS_VM_WRITE | PROCESS_VM_READ,
        FALSE,
        processId));
    if (!process) {
        std::wcerr << L"OpenProcess failed: " << GetLastError() << L"\n";
        return false;
    }

    const SIZE_T bytes = (path.size() + 1) * sizeof(wchar_t);
    void* remotePath = VirtualAllocEx(process.Get(), nullptr, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (remotePath == nullptr) {
        std::wcerr << L"VirtualAllocEx failed: " << GetLastError() << L"\n";
        return false;
    }

    if (!WriteProcessMemory(process.Get(), remotePath, path.c_str(), bytes, nullptr)) {
        std::wcerr << L"WriteProcessMemory failed: " << GetLastError() << L"\n";
        VirtualFreeEx(process.Get(), remotePath, 0, MEM_RELEASE);
        return false;
    }

    HMODULE kernel32 = GetModuleHandleW(L"kernel32.dll");
    auto loadLibrary = reinterpret_cast<LPTHREAD_START_ROUTINE>(
        GetProcAddress(kernel32, "LoadLibraryW"));
    UniqueHandle thread(CreateRemoteThread(process.Get(), nullptr, 0, loadLibrary, remotePath, 0, nullptr));
    if (!thread) {
        std::wcerr << L"CreateRemoteThread failed: " << GetLastError() << L"\n";
        VirtualFreeEx(process.Get(), remotePath, 0, MEM_RELEASE);
        return false;
    }

    WaitForSingleObject(thread.Get(), INFINITE);
    DWORD remoteResult = 0;
    GetExitCodeThread(thread.Get(), &remoteResult);
    VirtualFreeEx(process.Get(), remotePath, 0, MEM_RELEASE);

    // GetExitCodeThread is DWORD-sized even in a 64-bit process, so this is only
    // a null failure check for LoadLibraryW rather than a reliable module handle.
    if (remoteResult == 0) {
        std::wcerr << L"Remote LoadLibraryW returned null.\n";
        return false;
    }

    return true;
}

} // namespace olab::host
