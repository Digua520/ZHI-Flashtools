#pragma once
// ============================================================================
// UsbDevice —— 对 WinUSB 的最小可用封装
//
// 说明：
//   9008(EDL)、Fastboot、MTK BROM 这几个模式下，设备在 Windows 里都以
//   "裸 USB 接口"形式出现（走 WinUSB 驱动，而不是标准串口/HID驱动）。
//   本类只负责：打开设备句柄、找到 Bulk IN/OUT 端点、收发字节流。
//   具体每种协议（Sahara/Firehose/Fastboot命令）在各自的 Protocol 类里实现，
//   它们都依赖这一层，不直接碰 WinUSB API。
//
// 依赖：Windows SDK 自带的 <winusb.h> + <setupapi.h>，
//   工程需要链接 WinUsb.lib / SetupAPI.lib。
// ============================================================================

#include <windows.h>
#include <winusb.h>
#include <string>
#include <vector>
#include <cstdint>
#include <memory>

namespace zhi::usb {

// 一个可能匹配到的设备描述（供设备枚举器返回）
struct UsbDeviceInfo {
    std::wstring devicePath;   // SetupAPI 给出的设备符号链接路径，Open 时要用
    uint16_t     vid = 0;
    uint16_t     pid = 0;
    std::wstring friendlyName; // 用于在下拉框里展示，如 "Qualcomm HS-USB QDLoader 9008 (COM7)"
};

class UsbDevice {
public:
    UsbDevice() = default;
    ~UsbDevice();

    // 禁止拷贝，只允许移动（持有的是内核句柄）
    UsbDevice(const UsbDevice&) = delete;
    UsbDevice& operator=(const UsbDevice&) = delete;
    UsbDevice(UsbDevice&&) noexcept;
    UsbDevice& operator=(UsbDevice&&) noexcept;

    // 打开设备并初始化 WinUSB 接口，自动探测 Bulk IN/OUT 端点地址
    bool Open(const std::wstring& devicePath, std::string* errorOut = nullptr);
    void Close();
    bool IsOpen() const { return winusbHandle_ != nullptr; }

    // 批量写：9008 烧录/Fastboot下载大文件时会循环调用，chunkSize 建议 16KB~1MB
    bool BulkWrite(const uint8_t* data, size_t len, DWORD timeoutMs = 5000);

    // 批量读：返回实际读到的字节数，超时或设备拔出返回 false
    bool BulkRead(uint8_t* buffer, size_t bufferLen, size_t* bytesRead, DWORD timeoutMs = 5000);

    // 便捷重载：读到一个 vector 里，自动扩容
    bool BulkRead(std::vector<uint8_t>& out, size_t expectLen, DWORD timeoutMs = 5000);

    uint8_t BulkInPipe()  const { return pipeIn_; }
    uint8_t BulkOutPipe() const { return pipeOut_; }

private:
    HANDLE            fileHandle_   = INVALID_HANDLE_VALUE;
    WINUSB_INTERFACE_HANDLE winusbHandle_ = nullptr;
    uint8_t           pipeIn_  = 0;
    uint8_t           pipeOut_ = 0;

    bool FindBulkPipes(std::string* errorOut);
};

// ---------------------------------------------------------------------------
// 设备枚举器：用 SetupAPI 按 VID/PID 扫描当前挂载的 WinUSB 设备
// ---------------------------------------------------------------------------
class UsbDeviceEnumerator {
public:
    // 常见的目标 VID/PID：
    //   高通 9008 (EDL)         : 05C6:9008
    //   Fastboot（各厂商差异大）: 常见 18D1:D00D (Google), 也有厂商自定义
    //   MTK BROM/预下载模式      : 0E8D:0003 / 0E8D:2000 等
    struct VidPid { uint16_t vid; uint16_t pid; };

    static std::vector<UsbDeviceInfo> Enumerate(const std::vector<VidPid>& targets);

    // 注册热插拔通知（WM_DEVICECHANGE），hwnd 传主窗口句柄
    static bool RegisterHotplugNotification(HWND hwnd, HDEVNOTIFY* notifyHandleOut);
    static void UnregisterHotplugNotification(HDEVNOTIFY notifyHandle);
};

} // namespace zhi::usb
