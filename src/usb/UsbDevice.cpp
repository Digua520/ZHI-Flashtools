#include "UsbDevice.h"
#include "../core/Logger.h"
#include <setupapi.h>
#include <initguid.h>
#include <sstream>

#pragma comment(lib, "winusb.lib")
#pragma comment(lib, "setupapi.lib")

namespace zhi::usb {

// WinUSB 设备接口 GUID：设备的 .inf 驱动包安装时会注册这个 GUID，
// SetupAPI 靠它才能枚举到走 WinUSB 驱动的设备。
// {88BAE032-5A81-49f0-BC3D-A4FF138216D6} 是微软 WinUSB 官方示例使用的通用 GUID，
// 实际项目里应改成你自己驱动 inf 里声明的 DeviceInterfaceGUID。
DEFINE_GUID(GUID_DEVINTERFACE_WINUSB_GENERIC,
    0x88bae032, 0x5a81, 0x49f0, 0xbc, 0x3d, 0xa4, 0xff, 0x13, 0x82, 0x16, 0xd6);

UsbDevice::~UsbDevice() { Close(); }

UsbDevice::UsbDevice(UsbDevice&& other) noexcept {
    *this = std::move(other);
}

UsbDevice& UsbDevice::operator=(UsbDevice&& other) noexcept {
    if (this != &other) {
        Close();
        fileHandle_   = other.fileHandle_;
        winusbHandle_ = other.winusbHandle_;
        pipeIn_       = other.pipeIn_;
        pipeOut_      = other.pipeOut_;
        other.fileHandle_   = INVALID_HANDLE_VALUE;
        other.winusbHandle_ = nullptr;
    }
    return *this;
}

bool UsbDevice::Open(const std::wstring& devicePath, std::string* errorOut) {
    Close();

    fileHandle_ = CreateFileW(
        devicePath.c_str(),
        GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED,
        nullptr);

    if (fileHandle_ == INVALID_HANDLE_VALUE) {
        if (errorOut) *errorOut = "CreateFile 失败，错误码 " + std::to_string(GetLastError());
        return false;
    }

    if (!WinUsb_Initialize(fileHandle_, &winusbHandle_)) {
        if (errorOut) *errorOut = "WinUsb_Initialize 失败，错误码 " + std::to_string(GetLastError());
        CloseHandle(fileHandle_);
        fileHandle_ = INVALID_HANDLE_VALUE;
        return false;
    }

    if (!FindBulkPipes(errorOut)) {
        Close();
        return false;
    }

    zhi::Logger::Instance().Success("USB 设备已打开，BulkIN=0x" +
        std::to_string(pipeIn_) + " BulkOUT=0x" + std::to_string(pipeOut_));
    return true;
}

bool UsbDevice::FindBulkPipes(std::string* errorOut) {
    USB_INTERFACE_DESCRIPTOR ifaceDesc;
    if (!WinUsb_QueryInterfaceSettings(winusbHandle_, 0, &ifaceDesc)) {
        if (errorOut) *errorOut = "查询接口描述失败";
        return false;
    }

    for (UCHAR i = 0; i < ifaceDesc.bNumEndpoints; ++i) {
        WINUSB_PIPE_INFORMATION pipeInfo;
        if (!WinUsb_QueryPipe(winusbHandle_, 0, i, &pipeInfo)) continue;

        if (pipeInfo.PipeType == UsbdPipeTypeBulk) {
            // 端点地址最高位为1表示IN方向，0表示OUT方向（USB规范）
            if (USB_ENDPOINT_DIRECTION_IN(pipeInfo.PipeId)) {
                pipeIn_ = pipeInfo.PipeId;
            } else {
                pipeOut_ = pipeInfo.PipeId;
            }
        }
    }

    if (pipeIn_ == 0 || pipeOut_ == 0) {
        if (errorOut) *errorOut = "未找到成对的 Bulk IN/OUT 端点，设备可能不支持或驱动异常";
        return false;
    }
    return true;
}

void UsbDevice::Close() {
    if (winusbHandle_) {
        WinUsb_Free(winusbHandle_);
        winusbHandle_ = nullptr;
    }
    if (fileHandle_ != INVALID_HANDLE_VALUE) {
        CloseHandle(fileHandle_);
        fileHandle_ = INVALID_HANDLE_VALUE;
    }
    pipeIn_ = pipeOut_ = 0;
}

bool UsbDevice::BulkWrite(const uint8_t* data, size_t len, DWORD timeoutMs) {
    if (!IsOpen()) return false;

    WinUsb_SetPipePolicy(winusbHandle_, pipeOut_, PIPE_TRANSFER_TIMEOUT,
        sizeof(timeoutMs), &timeoutMs);

    ULONG written = 0;
    BOOL ok = WinUsb_WritePipe(winusbHandle_, pipeOut_,
        const_cast<PUCHAR>(data), static_cast<ULONG>(len), &written, nullptr);

    if (!ok || written != len) {
        zhi::Logger::Instance().Error("BulkWrite 失败/写入不完整，错误码 " +
            std::to_string(GetLastError()));
        return false;
    }
    return true;
}

bool UsbDevice::BulkRead(uint8_t* buffer, size_t bufferLen, size_t* bytesRead, DWORD timeoutMs) {
    if (!IsOpen()) return false;

    WinUsb_SetPipePolicy(winusbHandle_, pipeIn_, PIPE_TRANSFER_TIMEOUT,
        sizeof(timeoutMs), &timeoutMs);

    ULONG read = 0;
    BOOL ok = WinUsb_ReadPipe(winusbHandle_, pipeIn_,
        buffer, static_cast<ULONG>(bufferLen), &read, nullptr);

    if (!ok) {
        DWORD err = GetLastError();
        if (err == ERROR_SEM_TIMEOUT) {
            zhi::Logger::Instance().Warning("BulkRead 超时（设备无响应）");
        } else {
            zhi::Logger::Instance().Error("BulkRead 失败，错误码 " + std::to_string(err));
        }
        return false;
    }

    if (bytesRead) *bytesRead = read;
    return true;
}

bool UsbDevice::BulkRead(std::vector<uint8_t>& out, size_t expectLen, DWORD timeoutMs) {
    out.resize(expectLen);
    size_t got = 0;
    if (!BulkRead(out.data(), out.size(), &got, timeoutMs)) return false;
    out.resize(got);
    return true;
}

// ---------------------------------------------------------------------------
// 设备枚举实现
// ---------------------------------------------------------------------------
std::vector<UsbDeviceInfo> UsbDeviceEnumerator::Enumerate(const std::vector<VidPid>& targets) {
    std::vector<UsbDeviceInfo> results;

    HDEVINFO devInfoSet = SetupDiGetClassDevsW(
        &GUID_DEVINTERFACE_WINUSB_GENERIC,
        nullptr, nullptr,
        DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);

    if (devInfoSet == INVALID_HANDLE_VALUE) return results;

    SP_DEVICE_INTERFACE_DATA ifaceData{};
    ifaceData.cbSize = sizeof(ifaceData);

    for (DWORD index = 0; SetupDiEnumDeviceInterfaces(
            devInfoSet, nullptr, &GUID_DEVINTERFACE_WINUSB_GENERIC, index, &ifaceData);
         ++index) {

        DWORD requiredSize = 0;
        SetupDiGetDeviceInterfaceDetailW(devInfoSet, &ifaceData, nullptr, 0, &requiredSize, nullptr);
        if (requiredSize == 0) continue;

        std::vector<uint8_t> buffer(requiredSize);
        auto* detail = reinterpret_cast<PSP_DEVICE_INTERFACE_DETAIL_DATA_W>(buffer.data());
        detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);

        SP_DEVINFO_DATA devInfoData{};
        devInfoData.cbSize = sizeof(devInfoData);

        if (!SetupDiGetDeviceInterfaceDetailW(
                devInfoSet, &ifaceData, detail, requiredSize, nullptr, &devInfoData)) {
            continue;
        }

        std::wstring devicePath = detail->DevicePath;

        // 设备路径形如 \\?\usb#vid_05c6&pid_9008#... ，直接从路径里抠出 VID/PID，
        // 比调用 SetupDiGetDeviceRegistryProperty 更直接可靠。
        auto vidPos = devicePath.find(L"vid_");
        auto pidPos = devicePath.find(L"pid_");
        if (vidPos == std::wstring::npos || pidPos == std::wstring::npos) continue;

        uint16_t vid = static_cast<uint16_t>(wcstol(devicePath.substr(vidPos + 4, 4).c_str(), nullptr, 16));
        uint16_t pid = static_cast<uint16_t>(wcstol(devicePath.substr(pidPos + 4, 4).c_str(), nullptr, 16));

        for (const auto& t : targets) {
            if (t.vid == vid && t.pid == pid) {
                UsbDeviceInfo info;
                info.devicePath = devicePath;
                info.vid = vid;
                info.pid = pid;

                std::wstringstream ss;
                ss << L"VID_" << std::hex << vid << L"&PID_" << std::hex << pid;
                info.friendlyName = ss.str();

                results.push_back(std::move(info));
                break;
            }
        }
    }

    SetupDiDestroyDeviceInfoList(devInfoSet);
    return results;
}

bool UsbDeviceEnumerator::RegisterHotplugNotification(HWND hwnd, HDEVNOTIFY* notifyHandleOut) {
    DEV_BROADCAST_DEVICEINTERFACE_W filter{};
    filter.dbcc_size = sizeof(filter);
    filter.dbcc_devicetype = DBT_DEVTYP_DEVICEINTERFACE;
    filter.dbcc_classguid = GUID_DEVINTERFACE_WINUSB_GENERIC;

    HDEVNOTIFY h = RegisterDeviceNotificationW(
        hwnd, &filter, DEVICE_NOTIFY_WINDOW_HANDLE);

    if (!h) return false;
    if (notifyHandleOut) *notifyHandleOut = h;
    return true;
}

void UsbDeviceEnumerator::UnregisterHotplugNotification(HDEVNOTIFY notifyHandle) {
    if (notifyHandle) UnregisterDeviceNotification(notifyHandle);
}

} // namespace zhi::usb
