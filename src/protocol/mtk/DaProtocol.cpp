#include "DaProtocol.h"
#include "../../core/Logger.h"
#include <algorithm>
#include <cstring>

namespace zhi::protocol::mtk {

using zhi::Logger;

void DaProtocol::WriteU32BE(std::vector<uint8_t>& buf, uint32_t val) {
    buf.push_back(static_cast<uint8_t>(val >> 24));
    buf.push_back(static_cast<uint8_t>(val >> 16));
    buf.push_back(static_cast<uint8_t>(val >> 8));
    buf.push_back(static_cast<uint8_t>(val & 0xFF));
}

void DaProtocol::WriteU64BE(std::vector<uint8_t>& buf, uint64_t val) {
    for (int shift = 56; shift >= 0; shift -= 8) {
        buf.push_back(static_cast<uint8_t>(val >> shift));
    }
}

uint32_t DaProtocol::ReadU32BE(const uint8_t* buf) {
    return (static_cast<uint32_t>(buf[0]) << 24) | (static_cast<uint32_t>(buf[1]) << 16) |
           (static_cast<uint32_t>(buf[2]) << 8) | buf[3];
}

uint16_t DaProtocol::Checksum16(const uint8_t* data, size_t len) {
    uint16_t sum = 0;
    for (size_t i = 0; i < len; ++i) {
        sum ^= static_cast<uint16_t>(data[i]) << ((i % 2) * 8);
    }
    return sum;
}

bool DaProtocol::SendCommandByte(uint8_t cmd, std::string* errorOut) {
    if (!device_.BulkWrite(&cmd, 1)) {
        if (errorOut) *errorOut = "发送 DA 命令字节失败";
        return false;
    }
    uint8_t echo = 0;
    size_t got = 0;
    if (!device_.BulkRead(&echo, 1, &got, 3000) || got != 1) {
        if (errorOut) *errorOut = "等待 DA 命令回显超时";
        return false;
    }
    if (echo != cmd) {
        if (errorOut) *errorOut = "DA 命令回显不匹配（这通常说明 opcode 值不对，" 
            "参见 DaOpcodes 结构体注释，需要用真实DA核实命令编号）";
        return false;
    }
    return true;
}

bool DaProtocol::WritePartition(const DaPartitionTarget& target, const uint8_t* data, size_t len,
                                 ProgressCallback onProgress, std::string* errorOut) {
    if (!SendCommandByte(opcodes_.writePartition, errorOut)) return false;

    std::vector<uint8_t> params;
    WriteU64BE(params, target.physicalAddr);
    WriteU64BE(params, static_cast<uint64_t>(len));

    if (!device_.BulkWrite(params.data(), params.size())) {
        if (errorOut) *errorOut = "发送写入参数失败";
        return false;
    }

    // 设备确认参数合法，回一个32位状态字（0=OK）
    uint8_t statusBuf[4] = {0};
    size_t got = 0;
    if (!device_.BulkRead(statusBuf, 4, &got, 3000) || got != 4) {
        if (errorOut) *errorOut = "等待写入参数确认超时";
        return false;
    }
    if (ReadU32BE(statusBuf) != 0) {
        if (errorOut) *errorOut = "设备拒绝写入参数（分区 " + target.name + "），"
            "可能是地址/长度超出分区范围";
        return false;
    }

    // 分块传输镜像数据
    size_t sent = 0;
    uint16_t runningChecksum = 0;
    while (sent < len) {
        size_t thisChunk = std::min(chunkSize_, len - sent);
        if (!device_.BulkWrite(data + sent, thisChunk)) {
            if (errorOut) *errorOut = "分区 " + target.name + " 数据传输中断，已发送 " +
                std::to_string(sent) + " / " + std::to_string(len);
            return false;
        }
        // 累加校验和：每个分块单独算，最后异或到一起，避免大文件一次性算导致溢出语义混乱
        uint16_t chunkSum = Checksum16(data + sent, thisChunk);
        runningChecksum ^= chunkSum;

        sent += thisChunk;
        if (onProgress) onProgress(sent, len);
    }

    // 设备回报校验和，核对
    uint8_t checksumBuf[2] = {0};
    if (!device_.BulkRead(checksumBuf, 2, &got, 8000) || got != 2) {
        if (errorOut) *errorOut = "等待写入校验和响应超时";
        return false;
    }
    uint16_t deviceChecksum = (static_cast<uint16_t>(checksumBuf[0]) << 8) | checksumBuf[1];
    if (deviceChecksum != runningChecksum) {
        Logger::Instance().Warning("分区 " + target.name + " 校验和不一致：主机=0x" +
            std::to_string(runningChecksum) + " 设备=0x" + std::to_string(deviceChecksum) +
            "（若怀疑传输有误，建议重新写入这个分区，不要继续下一个分区）");
    }

    Logger::Instance().Success("分区 " + target.name + " 写入完成，" + std::to_string(len) + " 字节");
    return true;
}

bool DaProtocol::ReadPartition(const DaPartitionTarget& target, std::vector<uint8_t>* outData,
                                ProgressCallback onProgress, std::string* errorOut) {
    if (!SendCommandByte(opcodes_.readPartition, errorOut)) return false;

    std::vector<uint8_t> params;
    WriteU64BE(params, target.physicalAddr);
    WriteU64BE(params, target.length);

    if (!device_.BulkWrite(params.data(), params.size())) {
        if (errorOut) *errorOut = "发送读取参数失败";
        return false;
    }

    uint8_t statusBuf[4] = {0};
    size_t got = 0;
    if (!device_.BulkRead(statusBuf, 4, &got, 3000) || got != 4) {
        if (errorOut) *errorOut = "等待读取参数确认超时";
        return false;
    }
    if (ReadU32BE(statusBuf) != 0) {
        if (errorOut) *errorOut = "设备拒绝读取请求（分区 " + target.name + "）";
        return false;
    }

    outData->clear();
    outData->reserve(target.length);

    uint64_t received = 0;
    std::vector<uint8_t> chunk(chunkSize_);
    while (received < target.length) {
        size_t want = static_cast<size_t>(std::min<uint64_t>(chunkSize_, target.length - received));
        size_t chunkGot = 0;
        if (!device_.BulkRead(chunk.data(), want, &chunkGot, 10000)) {
            if (errorOut) *errorOut = "分区 " + target.name + " 读取中断，已读取 " +
                std::to_string(received);
            return false;
        }
        outData->insert(outData->end(), chunk.begin(), chunk.begin() + chunkGot);
        received += chunkGot;
        if (onProgress) onProgress(received, target.length);
    }

    Logger::Instance().Success("分区 " + target.name + " 读取完成，" +
        std::to_string(received) + " 字节");
    return true;
}

bool DaProtocol::FormatPartition(const DaPartitionTarget& target, std::string* errorOut) {
    if (!SendCommandByte(opcodes_.formatPartition, errorOut)) return false;

    std::vector<uint8_t> params;
    WriteU64BE(params, target.physicalAddr);
    WriteU64BE(params, target.length);

    if (!device_.BulkWrite(params.data(), params.size())) {
        if (errorOut) *errorOut = "发送格式化参数失败";
        return false;
    }

    uint8_t statusBuf[4] = {0};
    size_t got = 0;
    // 格式化可能比较慢（尤其大分区全擦），超时给长一点
    if (!device_.BulkRead(statusBuf, 4, &got, 30000) || got != 4) {
        if (errorOut) *errorOut = "等待格式化完成超时";
        return false;
    }
    if (ReadU32BE(statusBuf) != 0) {
        if (errorOut) *errorOut = "格式化分区 " + target.name + " 失败";
        return false;
    }

    Logger::Instance().Success("分区 " + target.name + " 已格式化");
    return true;
}

bool DaProtocol::Finish(bool autoReboot, std::string* errorOut) {
    if (!SendCommandByte(opcodes_.finish, errorOut)) return false;

    uint8_t flag = autoReboot ? 1 : 0;
    if (!device_.BulkWrite(&flag, 1)) {
        if (errorOut) *errorOut = "发送结束标志失败";
        return false;
    }

    Logger::Instance().Info(std::string("DA 会话结束") + (autoReboot ? "，设备即将重启" : ""));
    return true;
}

} // namespace zhi::protocol::mtk
