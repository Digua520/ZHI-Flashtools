#pragma once
// ============================================================================
// AdbProtocol —— Android Debug Bridge 传输层协议（AOSP system/core/adb 公开协议）
//
// 协议要点：
//   - 每条消息固定 24 字节头部 + 可变长度 payload
//   - 头部字段：command(4) arg0(4) arg1(4) data_length(4) data_crc32(4) magic(4)
//     magic = command ^ 0xFFFFFFFF，用来做基本校验
//   - 建立连接：主机发 CNXN，携带版本号+最大payload+"host::features=..."字符串，
//     设备回一条 CNXN 表示接受连接（未授权时会先收到 AUTH，需要 RSA 密钥签名，
//     这部分涉及主机私钥/设备端"允许调试"授权弹窗，是标准 ADB 安全机制，
//     不属于绕过范畴——用户需要在手机上手动点"允许"）
//   - 打开数据流：主机发 OPEN，arg0=本地流ID，payload 是要连接的服务名，
//     如 "shell:", "sync:", "reboot:" 等，设备回 OKAY 表示流建立成功
//   - 数据传输：WRTE 发送数据，收到对方 OKAY 才能发下一包（简单滑动窗口）
//   - 关闭流：CLSE
// ============================================================================

#include "../../usb/UsbDevice.h"
#include <string>
#include <vector>
#include <cstdint>

namespace zhi::protocol::adb {

constexpr uint32_t A_SYNC = 0x434e5953;
constexpr uint32_t A_CNXN = 0x4e584e43;
constexpr uint32_t A_OPEN = 0x4e45504f;
constexpr uint32_t A_OKAY = 0x59414b4f;
constexpr uint32_t A_CLSE = 0x45534c43;
constexpr uint32_t A_WRTE = 0x45545257;
constexpr uint32_t A_AUTH = 0x48545541;

// AUTH 消息的 arg0 取值（AOSP 协议定义）
constexpr uint32_t ADB_AUTH_TOKEN      = 1; // 设备发来的挑战数据
constexpr uint32_t ADB_AUTH_SIGNATURE  = 2; // 主机回发的签名
constexpr uint32_t ADB_AUTH_RSAPUBLICKEY = 3; // 主机回发的公钥（首次连接/签名被拒时用）

constexpr uint32_t A_VERSION = 0x01000000;
constexpr uint32_t MAX_PAYLOAD = 4096 * 64; // 256KB，与现代 adbd 保持一致

#pragma pack(push, 1)
struct AdbMessageHeader {
    uint32_t command;
    uint32_t arg0;
    uint32_t arg1;
    uint32_t dataLength;
    uint32_t dataCrc32;
    uint32_t magic;
};
#pragma pack(pop)

struct AdbMessage {
    AdbMessageHeader header{};
    std::vector<uint8_t> payload;
};

class AdbProtocol {
public:
    explicit AdbProtocol(zhi::usb::UsbDevice& device) : device_(device) {}

    // 建立 ADB 连接（CNXN握手 + AUTH签名认证），keyPath 是本地私钥文件路径
    // （不存在会自动生成），userAtHost 会显示在手机"允许调试"弹窗的设备指纹里，
    // 一般传类似 "unknown@zhi-flashtools" 的字符串即可。
    // deviceBanner 里是设备返回的 "device::ro.product.name=...;..." 字符串。
    bool Connect(const std::wstring& keyPath, const std::string& userAtHost,
                std::string* deviceBanner, std::string* errorOut = nullptr);

    // 打开一个服务流并等待 OKAY，返回本地/远端流 ID（后续 WRTE/CLSE 要用）
    bool OpenStream(const std::string& service, uint32_t* localIdOut, uint32_t* remoteIdOut,
                     std::string* errorOut = nullptr);

    // 在已打开的流上发送数据
    bool Write(uint32_t localId, uint32_t remoteId, const uint8_t* data, size_t len,
               std::string* errorOut = nullptr);

    // 读取一条消息（阻塞，直到超时）。timeoutMs 默认8秒，但等待用户在手机上
    // 点击"允许调试"确认框时（EncodePublicKey 之后）建议传更长的值，比如
    // 30000~60000，给用户留出反应时间。
    bool ReadMessage(AdbMessage* out, std::string* errorOut = nullptr, DWORD timeoutMs = 8000);

    bool CloseStream(uint32_t localId, uint32_t remoteId);

    // ---- 常用封装：走 shell: 服务执行一条命令，返回标准输出 ----
    bool Shell(const std::string& command, std::string* outputOut, std::string* errorOut = nullptr);

    // ---- 推送文件到设备（sync:服务，SEND/DATA/DONE子协议）----
    // remotePath 例如 "/sdcard/xxx.apk"，mode 是Unix权限位，默认 0644
    bool Push(const std::wstring& localPath, const std::string& remotePath,
              int mode = 0100644, std::string* errorOut = nullptr);

private:
    zhi::usb::UsbDevice& device_;
    uint32_t nextLocalId_ = 1;

    bool SendMessage(uint32_t command, uint32_t arg0, uint32_t arg1,
                      const uint8_t* data, size_t len, std::string* errorOut);
    static uint32_t Crc32(const uint8_t* data, size_t len);
};

} // namespace zhi::protocol::adb
