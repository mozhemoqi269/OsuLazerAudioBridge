#include "HostAsioDevices.h"

#include <Windows.h>

#include <asio.h>
#include <asiodrivers.h>
#include <iasiodrv.h>

#include <iostream>
#include <memory>
#include <vector>

extern IASIO* theAsioDriver;

namespace olab::host {
namespace {

struct AsioDriverInfo {
    std::wstring name;
    CLSID clsid {};
};

std::string WideToAnsi(const std::wstring& value)
{
    if (value.empty())
        return {};

    int length = WideCharToMultiByte(CP_ACP, 0, value.c_str(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    if (length <= 0)
        return {};

    std::string result(static_cast<std::size_t>(length), '\0');
    WideCharToMultiByte(CP_ACP, 0, value.c_str(), static_cast<int>(value.size()), result.data(), length, nullptr, nullptr);
    return result;
}

std::vector<AsioDriverInfo> EnumerateAsioDrivers()
{
    std::vector<AsioDriverInfo> drivers;
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\ASIO", 0, KEY_READ | KEY_WOW64_64KEY, &key) != ERROR_SUCCESS)
        return drivers;

    DWORD index = 0;
    while (true) {
        wchar_t name[256] {};
        DWORD nameLength = static_cast<DWORD>(std::size(name));
        const LSTATUS enumStatus = RegEnumKeyExW(key, index++, name, &nameLength, nullptr, nullptr, nullptr, nullptr);
        if (enumStatus == ERROR_NO_MORE_ITEMS)
            break;
        if (enumStatus != ERROR_SUCCESS)
            continue;

        HKEY driverKey = nullptr;
        if (RegOpenKeyExW(key, name, 0, KEY_READ, &driverKey) != ERROR_SUCCESS)
            continue;

        wchar_t clsidText[128] {};
        DWORD clsidBytes = sizeof(clsidText);
        DWORD type = 0;
        CLSID clsid {};
        if (RegQueryValueExW(driverKey, L"CLSID", nullptr, &type, reinterpret_cast<BYTE*>(clsidText), &clsidBytes) == ERROR_SUCCESS
            && (type == REG_SZ || type == REG_EXPAND_SZ)
            && SUCCEEDED(CLSIDFromString(clsidText, &clsid))) {
            drivers.push_back(AsioDriverInfo { name, clsid });
        }

        RegCloseKey(driverKey);
    }

    RegCloseKey(key);
    return drivers;
}

} // namespace

bool ListAsioDevices()
{
    const std::vector<AsioDriverInfo> drivers = EnumerateAsioDrivers();
    for (std::size_t i = 0; i < drivers.size(); i++) {
        LPOLESTR clsidText = nullptr;
        StringFromCLSID(drivers[i].clsid, &clsidText);
        std::wcout << L"[" << i << L"] " << drivers[i].name << L"\n"
                   << L"    " << (clsidText == nullptr ? L"" : clsidText) << L"\n";
        if (clsidText != nullptr)
            CoTaskMemFree(clsidText);
    }
    return !drivers.empty();
}

std::optional<std::wstring> ResolveAsioDriverName(const std::wstring& requestedDeviceId)
{
    const std::vector<AsioDriverInfo> drivers = EnumerateAsioDrivers();
    if (drivers.empty())
        return std::nullopt;

    if (!requestedDeviceId.empty()) {
        for (const AsioDriverInfo& driver : drivers) {
            if (_wcsicmp(driver.name.c_str(), requestedDeviceId.c_str()) == 0)
                return driver.name;
        }
    }

    return drivers.front().name;
}

bool OpenAsioControlPanel(const OutputConfig& config)
{
    const auto driverName = ResolveAsioDriverName(config.deviceId);
    if (!driverName) {
        std::wcerr << L"AudioMirrorError operation=\"NativeASIO.ResolveDriver\" message=\"No ASIO driver found. Use --list-asio-devices.\"\n";
        return false;
    }

    const std::string driverNameAnsi = WideToAnsi(*driverName);
    if (driverNameAnsi.empty()) {
        std::wcerr << L"AudioMirrorError operation=\"NativeASIO.DriverName\" message=\"Could not convert driver name.\"\n";
        return false;
    }

    auto asioDrivers = std::make_unique<AsioDrivers>();
    if (!asioDrivers->loadDriver(const_cast<char*>(driverNameAnsi.c_str())) || theAsioDriver == nullptr) {
        std::wcerr << L"AudioMirrorError operation=\"NativeASIO.LoadDriver\" driver=\"" << *driverName << L"\"\n";
        asioDrivers->removeCurrentDriver();
        theAsioDriver = nullptr;
        return false;
    }

    ASIODriverInfo info {};
    info.asioVersion = 2;
    info.sysRef = nullptr;
    if (theAsioDriver->init(&info) != ASIOTrue) {
        std::wcerr << L"AudioMirrorError operation=\"NativeASIO.Init\" driver=\"" << *driverName << L"\"\n";
        asioDrivers->removeCurrentDriver();
        theAsioDriver = nullptr;
        return false;
    }

    const ASIOError result = theAsioDriver->controlPanel();
    std::wcout << L"AsioControlPanelOpened driver=\"" << *driverName << L"\" result=" << static_cast<long>(result) << L"\n";
    asioDrivers->removeCurrentDriver();
    theAsioDriver = nullptr;
    return true;
}

} // namespace olab::host
