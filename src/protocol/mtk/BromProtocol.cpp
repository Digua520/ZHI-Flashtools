#include "BromProtocol.h"
#include "../../core/Logger.h"
#include <cstring>

namespace zhi::protocol::mtk {

using zhi::Logger;

void BromProtocol::WriteU16BE(std::vector<uint8_t>& buf, uint16_t val) {
    buf.push_back(static_cast<uint8_t>(val >> 8));
    buf.push_back(static_cast<uint8_t>(val & 0xFF));
}

void BromProtocol::WriteU32BE(std::vector<uint8_t>& buf, uint32_t val) {
    buf.push_back(static_cast<uint8_t>(val >> 24));
    buf.push_back(static_cast<uint8_t>(val >> 16));
    buf.push_back(static_cast<uint8_t>(val >> 8));
    buf.push_back(static_cast<uint8_t>(val & 0xFF));
}

uint16_t BromProtocol::ReadU16BE(const uint8_t* buf) {
    return (static_cast<uint16_t>(buf[0]) << 8) | buf[1];
}

uint32_t BromProtocol::ReadU32BE(const uint8_t* buf) {
    return (static_cast<uint32_t>(buf[0]) << 24) | (static_cast<uint32_t>(buf[1]) << 16) |
           (static_cast<uint32_t>(buf[2]) << 8) | buf[3];
}

bool BromProtocol::Handshake(int maxRetries, std::string* errorOut) {
    // 四步自动波特率探测：每步"发一个字节，期望收到该字节按位取反的回显"
    const uint8_t sendSeq[4] = {0xA0, 0x0A, 0x50, 0x05};

    for (int attempt = 0; attempt < maxRetries; ++attempt) {
        bool ok = true;

        for (uint8_t sendByte : sendSeq) {
            if (!device_.BulkWrite(&sendByte, 1, 1000)) {
                ok = false;
                break;
            }
            uint8_t recvByte = 0;
            size_t got = 0;
            if (!device_.BulkRead(&recvByte, 1, &got, 1000) || got != 1) {
                ok = false;
                break;
            }
            uint8_t expected = static_cast<uint8_t>(~sendByte);
            if (recvByte != expected) {
                ok = false;
                break;
            }
        }

        if (ok) {
            Logger::Instance().Success("BROM 握手成功（尝试 " + std::to_string(attempt + 1) + " 次）");
            return true;
        }
        // 握手失败很常见（设备可能还没进入 BROM 或时序没对齐），直接重试，不打印每次失败
    }

    if (errorOut) *errorOut = "BROM 握手失败，已重试 " + std::to_string(maxRetries) +
        " 次。请确认设备已进入 BROM/线刷模式（通常是断电后按住音量键插入数据线）";
    return false;
}

bool BromProtocol::SendCommandByte(uint8_t cmd, std::string* errorOut) {
    if (!device_.BulkWrite(&cmd, 1)) {
        if (errorOut) *errorOut = "发送命令字节失败: 0x" + std::to_string(cmd);
        return false;
    }
    uint8_t echo = 0;
    size_t got = 0;
    if (!device_.BulkRead(&echo, 1, &got, 3000) || got != 1) {
        if (errorOut) *errorOut = "等待命令回显超时";
        return false;
    }
    if (echo != cmd) {
        if (errorOut) *errorOut = "命令回显不匹配，发送0x" + std::to_string(cmd) +
            " 收到0x" + std::to_string(echo);
        return false;
    }
    return true;
}

bool BromProtocol::GetHwCode(HwInfo* outInfo, std::string* errorOut) {
    if (!SendCommandByte(static_cast<uint8_t>(BromCommand::GetHwCode), errorOut)) return false;

    uint8_t buf[8] = {0};
    size_t got = 0;
    // GET_HW_CODE 响应通常是 hw_code(2字节) + hw_sub_code 相关的状态字(2字节)
    if (!device_.BulkRead(buf, 4, &got, 3000) || got < 4) {
        if (errorOut) *errorOut = "读取 HW_CODE 响应失败";
        return false;
    }

    outInfo->hwCode = ReadU16BE(buf);
    outInfo->hwSubCode = ReadU16BE(buf + 2);

    Logger::Instance().Success("芯片识别: HW_CODE=0x" + std::to_string(outInfo->hwCode));

    // 部分 BROM 版本 GET_HW_CODE 之后紧跟版本信息，若无也不算错误，这里不强制读取
    return true;
}

bool BromProtocol::SendDA(const std::vector<uint8_t>& daData, uint32_t loadAddress,
                           uint32_t signatureLength, std::string* errorOut) {
    // !! 以下字节顺序是这类协议里最常见的写法，不同 BROM 版本可能有细微差异
    // （比如签名长度字段是2字节还是4字节），接入具体机型前务必先在测试机
    // 上抓一次真实交互确认，不要直接在唯一测试设备/生产设备上跑。
    if (!SendCommandByte(static_cast<uint8_t>(BromCommand::SendDA), errorOut)) return false;

    std::vector<uint8_t> params;
    WriteU32BE(params, loadAddress);
    WriteU32BE(params, static_cast<uint32_t>(daData.size()));
    WriteU32BE(params, signatureLength);

    if (!device_.BulkWrite(params.data(), params.size())) {
        if (errorOut) *errorOut = "发送 SEND_DA 参数失败";
        return false;
    }

    // 设备确认参数（通常回一个16位状态字，0表示OK）
    uint8_t statusBuf[2] = {0};
    size_t got = 0;
    if (!device_.BulkRead(statusBuf, 2, &got, 3000) || got != 2) {
        if (errorOut) *errorOut = "等待 SEND_DA 参数确认超时";
        return false;
    }
    uint16_t status = ReadU16BE(statusBuf);
    if (status != 0) {
        if (errorOut) *errorOut = "设备拒绝 SEND_DA 参数，status=0x" + std::to_string(status);
        return false;
    }

    // 分块发送 DA 数据本体，块大小取常见的 1KB（部分实现用更大的块，
    // 保守起见先用小块，稳定性优先，后续联调验证过可以调大提速）
    const size_t chunkSize = 1024;
    size_t sent = 0;
    uint16_t checksum = 0;
    while (sent < daData.size()) {
        size_t thisChunk = std::min(chunkSize, daData.size() - sent);
        if (!device_.BulkWrite(daData.data() + sent, thisChunk)) {
            if (errorOut) *errorOut = "DA 数据传输中断，已发送 " + std::to_string(sent);
            return false;
        }
        // 常见实现用16位异或校验和，主机自己也算一份，最后跟设备回报的比对
        for (size_t i = 0; i < thisChunk; ++i) {
            checksum ^= static_cast<uint16_t>(daData[sent + i]) << ((i % 2) * 8);
        }
        sent += thisChunk;
    }

    // 设备回报它算出来的校验和，双方要一致
    uint8_t checksumBuf[2] = {0};
    if (!device_.BulkRead(checksumBuf, 2, &got, 5000) || got != 2) {
        if (errorOut) *errorOut = "等待 DA 校验和响应超时";
        return false;
    }
    uint16_t deviceChecksum = ReadU16BE(checksumBuf);
    if (deviceChecksum != checksum) {
        Logger::Instance().Warning("DA 校验和不一致：主机计算=0x" + std::to_string(checksum) +
            " 设备回报=0x" + std::to_string(deviceChecksum) +
            "（校验算法可能因BROM版本而异，若确认传输完整可继续，但建议先核实）");
    }

    // 最终状态确认
    uint8_t finalStatus[2] = {0};
    if (!device_.BulkRead(finalStatus, 2, &got, 3000) || got != 2) {
        if (errorOut) *errorOut = "等待 SEND_DA 最终状态超时";
        return false;
    }
    if (ReadU16BE(finalStatus) != 0) {
        if (errorOut) *errorOut = "SEND_DA 最终状态非成功";
        return false;
    }

    Logger::Instance().Success("DA 已发送到设备内存，大小 " + std::to_string(daData.size()) + " 字节");
    return true;
}

bool BromProtocol::JumpDA(uint32_t loadAddress, std::string* errorOut) {
    if (!SendCommandByte(static_cast<uint8_t>(BromCommand::JumpDA), errorOut)) return false;

    std::vector<uint8_t> addr;
    WriteU32BE(addr, loadAddress);
    if (!device_.BulkWrite(addr.data(), addr.size())) {
        if (errorOut) *errorOut = "发送 JUMP_DA 地址失败";
        return false;
    }

    uint8_t statusBuf[2] = {0};
    size_t got = 0;
    if (!device_.BulkRead(statusBuf, 2, &got, 3000) || got != 2) {
        if (errorOut) *errorOut = "等待 JUMP_DA 状态超时";
        return false;
    }
    if (ReadU16BE(statusBuf) != 0) {
        if (errorOut) *errorOut = "JUMP_DA 失败";
        return false;
    }

    Logger::Instance().Success("已跳转执行 DA，BROM 阶段结束，后续通信切换为 DA 协议");
    return true;
}

} // namespace zhi::protocol::mtk
