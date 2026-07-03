#pragma once
// ============================================================================
// SaharaProtocol —— 高通 EDL 模式下的引导加载协议
//
// 设备刚进 9008(EDL) 时是"裸机"状态，只认 Sahara 协议。主机的任务是
// 通过 Sahara 把 Firehose Loader（一个由芯片厂商/OEM签名的 .elf/.mbn 文件）
// 传给设备并启动它，之后才切换到 Firehose 协议做实际的分区读写。
//
// 握手流程：
//   1. 设备上电即主动发 HELLO 包（携带协议版本、期望的 command_packet_length 等）
//   2. 主机回 HELLO-RESPONSE，status = 0 (SAHARA_MODE_IMAGE_TX_PENDING)
//   3. 设备发 READ_DATA 包，指定要读取 loader 文件的 offset + length
//   4. 主机把 loader 文件对应偏移的数据块发过去（循环，直到设备发 END_OF_IMAGE_TRANSFER）
//   5. 设备发 END_OF_IMAGE_TRANSFER，主机回 END_OF_IMAGE_TRANSFER 确认（status=0表示成功）
//   6. 设备发 DONE，主机回 DONE_RESP，握手完成，loader 已在设备上运行，
//      后续通信切换为 Firehose（XML协议）
// ============================================================================

#include "../../usb/UsbDevice.h"
#include <string>
#include <vector>
#include <map>
#include <cstdint>

namespace zhi::protocol::edl {

// Sahara 命令 ID（协议规范定义）
enum class SaharaCommand : uint32_t {
    Hello               = 0x01,
    HelloResponse       = 0x02,
    ReadData            = 0x03,
    EndOfImageTransfer  = 0x04,
    Done                = 0x05,
    DoneResponse        = 0x06,
    Reset               = 0x07,
    ResetResponse       = 0x08,
    // 64位地址变体（部分新平台使用）
    Read64Data          = 0x12,
};

#pragma pack(push, 1)
struct SaharaHeader {
    uint32_t command;
    uint32_t length;   // 整包长度（含header）
};

struct SaharaHelloPacket {
    SaharaHeader header;
    uint32_t version;
    uint32_t versionCompatible;
    uint32_t maxCommandPacketLength;
    uint32_t mode;
    uint32_t reserved[6];
};

struct SaharaHelloResponsePacket {
    SaharaHeader header;
    uint32_t version;
    uint32_t versionCompatible;
    uint32_t status;
    uint32_t mode;
    uint32_t reserved[6];
};

struct SaharaReadDataPacket {
    SaharaHeader header;
    uint32_t imageId;
    uint32_t offset;
    uint32_t length;
};

struct SaharaEndOfImageTransferPacket {
    SaharaHeader header;
    uint32_t imageId;
    uint32_t status;
};
#pragma pack(pop)

class SaharaProtocol {
public:
    explicit SaharaProtocol(zhi::usb::UsbDevice& device) : device_(device) {}

    // 执行完整握手：把 loaderPath 指向的 firehose loader 文件发送给设备。
    // 成功返回后，设备已经在跑 loader，可以切换到 FirehoseProtocol 了。
    bool PerformHandshake(const std::wstring& loaderPath, std::string* errorOut = nullptr);

    // 多镜像握手：部分厂商（比如欧加/OPPO系）的 PBL 在 Sahara 阶段不止要一个
    // loader 文件，还会陆续用不同的 imageId 请求额外的镜像（常见的是数字签名
    // 摘要文件 digest、以及对该摘要的签名文件 signature，具体命名和用途因
    // 厂商/平台而异）。
    //
    // !! 重要老实话 !!
    // Sahara 协议规范本身没有公开规定"哪个 imageId 对应哪个文件"，这是厂商
    // PBL 固件私有的约定，网上也没有可靠的公开文档。这里的实现策略是：
    // 按 imagePathsInOrder 给的顺序，"设备第一次请求到的新 imageId 分配第一个
    // 文件，第二个新 imageId 分配第二个文件，以此类推"——这是一个合理的猜测
    // （多数厂商按 loader→digest→signature 的顺序请求），但没有真机验证过，
    // 如果顺序不对，握手会在某一步卡住或被设备拒绝，需要用抓包工具确认真实
    // 的 imageId 分配规律后再调整。
    bool PerformHandshakeMultiImage(const std::vector<std::wstring>& imagePathsInOrder,
                                     std::string* errorOut = nullptr);

private:
    zhi::usb::UsbDevice& device_;

    bool WaitHello(SaharaHelloPacket* out, std::string* errorOut);
    bool SendHelloResponse(std::string* errorOut);
    bool ServeReadRequests(const std::vector<uint8_t>& loaderData, std::string* errorOut);

    // 多镜像版本的服务循环：imageIdToData 是"设备已经问过的imageId -> 对应文件数据"
    // 的映射，pendingFiles 是还没分配给任何imageId的文件队列（按用户给的顺序）。
    // 遇到一个没见过的 imageId 时，从 pendingFiles 队首取一个文件分配给它。
    bool ServeReadRequestsMultiImage(std::vector<std::vector<uint8_t>>& pendingFiles,
                                      std::string* errorOut);
};

} // namespace zhi::protocol::edl
