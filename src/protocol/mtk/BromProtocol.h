#pragma once
// ============================================================================
// BromProtocol —— 联发科 BootROM (BROM) / Preloader 握手协议
//
// 依据：这套握手序列是社区工具（如开源项目 mtkclient）多年逆向公开的
// 通用协议，不是某个特定机型的私有协议。核心思路：
//
//   1. 自动波特率探测握手：设备刚进 BROM 模式时，主机连续发送几个固定字节，
//      设备对每个字节回一个"按位取反"的响应，用来确认双方通信参数一致：
//         发 0xA0 -> 收 0x5F   (0xA0 ^ 0xFF = 0x5F)
//         发 0x0A -> 收 0xF5
//         发 0x50 -> 收 0xAF
//         发 0x05 -> 收 0xFA
//      四步都对上才算握手成功，任何一步不对就要重试（设备可能还没准备好）。
//
//   2. 握手成功后，命令层是"单字节命令，设备原样回显该字节，然后返回数据"：
//      主机发命令字节 -> 设备回显同一个字节（确认收到）-> 设备返回响应数据。
//
//   3. 典型流程：GET_HW_CODE（读取芯片型号）→ SEND_DA（发送 Download Agent，
//      即 MTK 自己的第二阶段引导程序）→ JUMP_DA（跳转执行 DA）。
//      DA 跑起来之后，真正的分区读写走 DA 自己的协议（不是 BROM 命令了），
//      这部分协议 DA 版本/机型差异很大，本文件先只覆盖到 JUMP_DA 为止。
//
// !! 重要提醒 !!
//   不同芯片平台（MT6765/MT6768/MT6889/天玑系列等）在 SEND_DA 阶段的具体
//   参数（DA加载地址、签名长度、分块大小等）差异较大，本实现给出的是最
//   通用的骨架和最常见的参数值，接入具体机型前务必先在测试机上验证，
//   不要直接对生产设备/唯一测试机使用未验证过的 SEND_DA 参数。
// ============================================================================

#include "../../usb/UsbDevice.h"
#include <string>
#include <vector>
#include <cstdint>

namespace zhi::protocol::mtk {

// BROM 单字节命令（社区工具里常见的命名和取值）
enum class BromCommand : uint8_t {
    GetHwCode       = 0xFD,
    GetHwSwVer      = 0xFC,
    GetTargetConfig = 0xD8,
    SendDA          = 0xD7,
    JumpDA          = 0xD5,
    ReadReg16       = 0xD0,
    WriteReg16      = 0xD1,
    Uart1LogEn      = 0xDB,
};

struct HwInfo {
    uint16_t hwCode = 0;     // 芯片型号ID，如 0x0766 对应某型号（需查对照表）
    uint16_t hwSubCode = 0;
    uint16_t hwVersion = 0;
    uint16_t swVersion = 0;
};

class BromProtocol {
public:
    explicit BromProtocol(zhi::usb::UsbDevice& device) : device_(device) {}

    // 自动波特率探测握手，成功后才能发命令。maxRetries 因为设备刚上电时
    // BROM 可能还没准备好接收，允许短暂重试。
    bool Handshake(int maxRetries = 20, std::string* errorOut = nullptr);

    // 读取芯片硬件信息（型号/版本），常用于 UI 显示"识别到芯片: MT6765"这类信息
    bool GetHwCode(HwInfo* outInfo, std::string* errorOut = nullptr);

    // 发送 Download Agent 并跳转执行。daData 是完整的 DA 文件内容，
    // loadAddress 是 DA 要被加载到的内存地址（因机型而异，需要参考对应
    // 平台的 DA 配置或 scatter 文件里的说明）。
    bool SendDA(const std::vector<uint8_t>& daData, uint32_t loadAddress,
                uint32_t signatureLength, std::string* errorOut = nullptr);

    bool JumpDA(uint32_t loadAddress, std::string* errorOut = nullptr);

private:
    zhi::usb::UsbDevice& device_;

    // 发一个命令字节，验证设备原样回显，返回是否成功
    bool SendCommandByte(uint8_t cmd, std::string* errorOut);

    // 16位大端写入/读取（BROM 协议里的多字节字段基本都是大端序）
    static void WriteU16BE(std::vector<uint8_t>& buf, uint16_t val);
    static void WriteU32BE(std::vector<uint8_t>& buf, uint32_t val);
    static uint16_t ReadU16BE(const uint8_t* buf);
    static uint32_t ReadU32BE(const uint8_t* buf);
};

} // namespace zhi::protocol::mtk
