#include "FirehoseProtocol.h"
#include "../../core/Logger.h"
#include <sstream>
#include <cstring>
#include <algorithm>

namespace zhi::protocol::edl {

using zhi::Logger;

bool FirehoseProtocol::SendXmlCommand(const std::string& xml, std::string* errorOut) {
    std::string packet = "<?xml version=\"1.0\" encoding=\"UTF-8\" ?><data>" + xml + "</data>";
    if (!device_.BulkWrite(reinterpret_cast<const uint8_t*>(packet.data()), packet.size())) {
        if (errorOut) *errorOut = "发送 Firehose XML 指令失败";
        return false;
    }
    return true;
}

bool FirehoseProtocol::ReadAckResponse(std::string* errorOut, std::string* rawXmlOut) {
    // Firehose 响应 XML 一般不超过几百字节，用较大缓冲区一次性读取即可；
    // 若设备把 <response> 和后续 <log> 分成多包发送，这里做简单的循环拼接。
    std::string accumulated;
    for (int attempt = 0; attempt < 8; ++attempt) {
        uint8_t buf[4096] = {0};
        size_t got = 0;
        if (!device_.BulkRead(buf, sizeof(buf), &got, 8000)) {
            if (errorOut) *errorOut = "等待 Firehose 响应超时";
            return false;
        }
        accumulated.append(reinterpret_cast<char*>(buf), got);

        if (accumulated.find("<response") != std::string::npos) break;
    }

    if (rawXmlOut) *rawXmlOut = accumulated;

    if (accumulated.find("value=\"ACK\"") != std::string::npos) {
        return true;
    }
    if (accumulated.find("value=\"NACK\"") != std::string::npos) {
        // 尝试把 NACK 附带的说明文字摘出来，方便直接显示在日志面板
        auto pos = accumulated.find("<log value=\"");
        if (pos != std::string::npos) {
            auto start = pos + strlen("<log value=\"");
            auto end = accumulated.find("\"", start);
            if (errorOut) *errorOut = "设备拒绝(NACK): " + accumulated.substr(start, end - start);
        } else if (errorOut) {
            *errorOut = "设备拒绝(NACK)，原始响应: " + accumulated;
        }
        return false;
    }

    if (errorOut) *errorOut = "未识别的 Firehose 响应: " + accumulated;
    return false;
}

bool FirehoseProtocol::Configure(uint32_t maxPayloadSize, std::string* errorOut) {
    sessionMaxPayload_ = maxPayloadSize;

    std::ostringstream xml;
    xml << "<configure MemoryName=\"eMMC\" "
        << "MaxPayloadSizeToTargetInBytes=\"" << maxPayloadSize << "\" "
        << "MaxPayloadSizeFromTargetInBytes=\"" << maxPayloadSize << "\" "
        << "verbose=\"0\" ZLPAwareHost=\"1\" SkipStorageInit=\"0\"/>";

    if (!SendXmlCommand(xml.str(), errorOut)) return false;

    std::string raw;
    if (!ReadAckResponse(errorOut, &raw)) return false;

    Logger::Instance().Success("Firehose Configure 成功");
    return true;
}

bool FirehoseProtocol::Program(const ProgramEntry& entry, const uint8_t* imageData, size_t imageLen,
                                std::function<void(uint64_t,uint64_t)> onProgress,
                                std::string* errorOut) {
    // 校验镜像大小和分区扇区数是否吻合，避免写越界（这一步在真实工具里非常关键）
    uint64_t expectedBytes = entry.numSectors * entry.sectorSize;
    if (imageLen > expectedBytes) {
        if (errorOut) *errorOut = "镜像文件(" + std::to_string(imageLen) +
            " 字节)大于分区容量(" + std::to_string(expectedBytes) + " 字节)，已中止，避免越界写入";
        return false;
    }

    std::ostringstream xml;
    xml << "<program SECTOR_SIZE_IN_BYTES=\"" << entry.sectorSize << "\" "
        << "num_partition_sectors=\"" << entry.numSectors << "\" "
        << "physical_partition_number=\"" << static_cast<int>(entry.physicalPartitionNumber) << "\" "
        << "start_sector=\"" << entry.startSector << "\" "
        << "filename=\"" << entry.filename << "\" "
        << "label=\"" << entry.label << "\"/>";

    if (!SendXmlCommand(xml.str(), errorOut)) return false;

    std::string raw;
    if (!ReadAckResponse(errorOut, &raw)) return false; // 设备确认参数合法，准备好接收数据

    // 按 sessionMaxPayload_ 分块发送原始镜像数据；不足一个扇区的部分要 padding 到扇区边界
    uint64_t totalToSend = ((imageLen + entry.sectorSize - 1) / entry.sectorSize) * entry.sectorSize;
    std::vector<uint8_t> padded(totalToSend, 0);
    memcpy(padded.data(), imageData, imageLen);

    uint64_t sent = 0;
    while (sent < totalToSend) {
        uint64_t thisChunk = std::min<uint64_t>(sessionMaxPayload_, totalToSend - sent);
        if (!device_.BulkWrite(padded.data() + sent, thisChunk)) {
            if (errorOut) *errorOut = "分区数据传输中断，已发送 " +
                std::to_string(sent) + " / " + std::to_string(totalToSend);
            return false;
        }
        sent += thisChunk;
        if (onProgress) onProgress(sent, totalToSend);
    }

    // 数据发送完毕，设备会再回一条最终 ACK/NACK 确认写入结果
    if (!ReadAckResponse(errorOut, &raw)) return false;

    Logger::Instance().Success("分区 " + entry.label + " 写入完成 (" +
        std::to_string(totalToSend) + " 字节)");
    return true;
}

bool FirehoseProtocol::Read(const ProgramEntry& entry, std::vector<uint8_t>* outData,
                             std::function<void(uint64_t,uint64_t)> onProgress,
                             std::string* errorOut) {
    std::ostringstream xml;
    xml << "<read SECTOR_SIZE_IN_BYTES=\"" << entry.sectorSize << "\" "
        << "num_partition_sectors=\"" << entry.numSectors << "\" "
        << "physical_partition_number=\"" << static_cast<int>(entry.physicalPartitionNumber) << "\" "
        << "start_sector=\"" << entry.startSector << "\" "
        << "filename=\"" << entry.label << ".bin\"/>";

    if (!SendXmlCommand(xml.str(), errorOut)) return false;

    std::string raw;
    if (!ReadAckResponse(errorOut, &raw)) return false;

    uint64_t totalToRead = entry.numSectors * entry.sectorSize;
    outData->clear();
    outData->reserve(totalToRead);

    uint64_t got = 0;
    std::vector<uint8_t> chunk(sessionMaxPayload_);
    while (got < totalToRead) {
        size_t chunkGot = 0;
        size_t want = static_cast<size_t>(std::min<uint64_t>(sessionMaxPayload_, totalToRead - got));
        if (!device_.BulkRead(chunk.data(), want, &chunkGot, 10000)) {
            if (errorOut) *errorOut = "读取分区数据中断，已读取 " + std::to_string(got);
            return false;
        }
        outData->insert(outData->end(), chunk.begin(), chunk.begin() + chunkGot);
        got += chunkGot;
        if (onProgress) onProgress(got, totalToRead);
    }

    // 读取完毕后设备也会发一条 ACK 确认
    if (!ReadAckResponse(errorOut, &raw)) return false;

    Logger::Instance().Success("分区 " + entry.label + " 读取完成 (" +
        std::to_string(got) + " 字节)");
    return true;
}

bool FirehoseProtocol::EraseLun(uint8_t physicalPartitionNumber, std::string* errorOut) {
    std::ostringstream xml;
    xml << "<erase physical_partition_number=\"" << static_cast<int>(physicalPartitionNumber) << "\"/>";

    if (!SendXmlCommand(xml.str(), errorOut)) return false;

    std::string raw;
    // 全盘擦除耗时可能较长，超时时间在 ReadAckResponse 内部固定为8秒/次、
    // 循环最多8次，即约64秒；实际项目里这里应该做成可配置的长超时。
    if (!ReadAckResponse(errorOut, &raw)) return false;

    Logger::Instance().Warning("LUN " + std::to_string(physicalPartitionNumber) + " 已全部擦除");
    return true;
}

bool FirehoseProtocol::Patch(uint32_t sectorSize, uint64_t byteOffset, const std::string& filename,
                              uint8_t physicalPartitionNumber, uint32_t sizeInBytes,
                              uint64_t startSector, const std::string& value,
                              const std::string& what, std::string* errorOut) {
    std::ostringstream xml;
    xml << "<patch SECTOR_SIZE_IN_BYTES=\"" << sectorSize << "\" "
        << "byte_offset=\"" << byteOffset << "\" "
        << "filename=\"" << filename << "\" "
        << "physical_partition_number=\"" << static_cast<int>(physicalPartitionNumber) << "\" "
        << "size_in_bytes=\"" << sizeInBytes << "\" "
        << "start_sector=\"" << startSector << "\" "
        << "value=\"" << value << "\" "
        << "what=\"" << what << "\"/>";

    if (!SendXmlCommand(xml.str(), errorOut)) return false;

    std::string raw;
    if (!ReadAckResponse(errorOut, &raw)) return false;

    Logger::Instance().Success("Patch 应用成功: " + what);
    return true;
}

bool FirehoseProtocol::ReadGpt(uint8_t physicalPartitionNumber, std::vector<uint8_t>* outData,
                                std::string* errorOut) {
    ProgramEntry gptEntry;
    gptEntry.physicalPartitionNumber = physicalPartitionNumber;
    gptEntry.startSector = 0;
    gptEntry.numSectors = 6; // 主GPT头(1扇区) + 分区表(通常32条目占约4扇区)，多读1扇区做余量
    gptEntry.sectorSize = 4096;
    gptEntry.label = "gpt_main" + std::to_string(physicalPartitionNumber);

    return Read(gptEntry, outData, nullptr, errorOut);
}

bool FirehoseProtocol::Reset(std::string* errorOut) {
    if (!SendXmlCommand("<power value=\"reset\"/>", errorOut)) return false;
    std::string raw;
    // 复位后设备可能不再回包（USB直接断开），这里不强制要求收到ACK
    ReadAckResponse(errorOut, &raw);
    Logger::Instance().Info("已发送复位指令，设备即将重启");
    return true;
}

bool FirehoseProtocol::GetStorageInfo(uint8_t physicalPartitionNumber, StorageInfo* outInfo,
                                       std::string* errorOut) {
    std::ostringstream xml;
    xml << "<getstorageinfo physical_partition_number=\""
        << static_cast<int>(physicalPartitionNumber) << "\"/>";

    if (!SendXmlCommand(xml.str(), errorOut)) return false;

    std::string raw;
    if (!ReadAckResponse(errorOut, &raw)) return false;

    outInfo->rawJson = raw;

    // 设备把存储信息塞在 <log value="...JSON..."/> 里，不同芯片平台字段名
    // 略有差异，常见的是 "total_blocks" + "block_size"（eMMC）或
    // "num_physical" 之类（UFS多LUN）。这里只做最常见的 "total_blocks" /
    // "block_size" 提取，抓不到就返回失败，调用方需要打日志排查具体机型的
    // 实际返回格式（建议先用 Logger::HexDump 或直接打印 rawJson 看一次）。
    auto extractNumber = [&raw](const std::string& key) -> uint64_t {
        auto pos = raw.find("\"" + key + "\"");
        if (pos == std::string::npos) return 0;
        pos = raw.find(':', pos);
        if (pos == std::string::npos) return 0;
        return strtoull(raw.c_str() + pos + 1, nullptr, 10);
    };

    outInfo->totalBlocks = extractNumber("total_blocks");
    outInfo->blockSize = static_cast<uint32_t>(extractNumber("block_size"));

    if (outInfo->totalBlocks == 0) {
        if (errorOut) *errorOut = "未能从设备响应中解析出 total_blocks，"
            "请检查 rawJson 字段确认该机型的实际字段名";
        Logger::Instance().Warning("GetStorageInfo 原始响应: " + raw);
        return false;
    }

    Logger::Instance().Success("磁盘总容量: " + std::to_string(outInfo->totalBlocks) +
        " 块 x " + std::to_string(outInfo->blockSize) + " 字节");
    return true;
}

} // namespace zhi::protocol::edl
