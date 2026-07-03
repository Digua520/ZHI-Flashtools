#pragma once
// ============================================================================
// FastbootProtocol —— Google 开源 Fastboot 协议的最小实现
//
// 协议要点（AOSP system/core/fastboot 里的公开协议）：
//   - 主机发送 ASCII 命令，格式如 "getvar:product" / "flash:boot" / "erase:cache"
//   - 命令长度不能超过 64 字节（USB 单次传输限制），必须一次性通过 BulkOUT 发出
//   - 设备响应以 4 字节前缀区分类型：
//       "OKAY" + 可选信息  → 命令成功
//       "FAIL" + 错误信息  → 命令失败
//       "DATA" + 8位十六进制长度 → 设备准备接收/发送指定长度的数据块
//       "INFO" + 信息文本  → 中间过程信息，需要继续等待下一条响应
//   - 大文件下载（flash分区）走两步：
//       1. 主机发 "download:<8位hex长度>"，设备回 DATA，主机把原始数据流写进去
//       2. 主机发 "flash:<分区名>"，设备把刚下载的数据写入目标分区
// ============================================================================

#include "../../usb/UsbDevice.h"
#include <string>
#include <vector>
#include <utility>
#include <functional>

namespace zhi::protocol::fastboot {

enum class ResponseType { Okay, Fail, Data, Info, Unknown };

struct FastbootResponse {
    ResponseType type = ResponseType::Unknown;
    std::string  message;   // OKAY/FAIL/INFO 后面跟的文本
    uint32_t     dataLen = 0; // DATA 响应携带的十六进制长度
};

// 进度回调：(已传输字节数, 总字节数)
using ProgressCallback = std::function<void(uint64_t sent, uint64_t total)>;

class FastbootProtocol {
public:
    explicit FastbootProtocol(zhi::usb::UsbDevice& device) : device_(device) {}

    // 发送一条命令并读取一条（或多条 INFO 之后的最终）响应
    bool SendCommand(const std::string& command, FastbootResponse* outResponse,
                      std::string* errorOut = nullptr);

    // ---- 常用封装 ----
    bool GetVar(const std::string& varName, std::string* valueOut);          // getvar:xxx

    // getvar:all 会让设备通过一连串 INFO 消息把所有变量吐出来（包括
    // partition-type:xxx / partition-size:xxx 这些和分区相关的信息），
    // 这里统一收集解析成 key-value 列表，供 UI 表格直接展示
    bool GetAllVars(std::vector<std::pair<std::string, std::string>>* varsOut);
    bool Reboot();                                                          // reboot
    bool RebootBootloader();                                                // reboot-bootloader
    bool RebootFastbootD();                                                 // reboot-fastboot
    bool RebootEdl();                                                       // oem edl / reboot-edl（厂商差异较大，见实现注释）
    bool Erase(const std::string& partition);                               // erase:xxx
    bool SetActiveSlot(char slot);                                          // set_active:a / set_active:b

    // 下载一段内存数据到设备缓冲区（flash 前置步骤）
    bool Download(const uint8_t* data, size_t len, ProgressCallback onProgress = nullptr,
                  std::string* errorOut = nullptr);

    // 把已下载的数据写入指定分区
    bool Flash(const std::string& partition, std::string* errorOut = nullptr);

    // 组合操作：读文件 -> Download -> Flash，供 UI 层"写入分区"按钮直接调用
    bool FlashFile(const std::string& partition, const std::wstring& imagePath,
                   ProgressCallback onProgress = nullptr, std::string* errorOut = nullptr);

private:
    zhi::usb::UsbDevice& device_;

    bool ReadResponse(FastbootResponse* out, std::string* errorOut);
    static ResponseType ParsePrefix(const char* buf4);
};

} // namespace zhi::protocol::fastboot
