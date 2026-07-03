#pragma once
// ============================================================================
// FirehoseProtocol —— Sahara 握手完成后，实际做分区读写用的协议
//
// Firehose 是基于 XML 的请求/响应协议：
//   主机发一个 <?xml ...?><data><program .../></data> 这样的 XML 指令，
//   设备执行后回一个 <response value="ACK"/> 或 <response value="NACK"/>，
//   部分命令（如 program/read）在 ACK 之后还会紧跟一个 <log> 元素说明详情，
//   然后才是真正的原始扇区数据（program 是主机发数据，read 是设备发数据）。
//
// 本实现用最简单的字符串拼接/查找来生成和解析 XML（够用且没有额外依赖）。
// 如果后续要支持更复杂的 patch/UFS 相关命令，建议换成 pugixml 做正规解析，
// 避免手写字符串匹配在复杂属性组合下出错。
// ============================================================================

#include "../../usb/UsbDevice.h"
#include "EdlTypes.h"
#include <string>
#include <vector>
#include <cstdint>
#include <functional>

namespace zhi::protocol::edl {

class FirehoseProtocol {
public:
    explicit FirehoseProtocol(zhi::usb::UsbDevice& device) : device_(device) {}

    // 配置阶段：告知设备本次会话用的 MaxPayloadSizeToTargetInBytes 等参数，
    // 必须在 program/read 之前调用一次
    bool Configure(uint32_t maxPayloadSize = 1048576, std::string* errorOut = nullptr);

    // 写入一个分区（program 命令）：先发 XML 描述，再发原始扇区数据
    bool Program(const ProgramEntry& entry, const uint8_t* imageData, size_t imageLen,
                 std::function<void(uint64_t,uint64_t)> onProgress = nullptr,
                 std::string* errorOut = nullptr);

    // 读取一个分区到内存（用于备份）
    bool Read(const ProgramEntry& entry, std::vector<uint8_t>* outData,
              std::function<void(uint64_t,uint64_t)> onProgress = nullptr,
              std::string* errorOut = nullptr);

    // 擦除指定 LUN 的整个磁盘（谨慎：全盘擦除）
    bool EraseLun(uint8_t physicalPartitionNumber, std::string* errorOut = nullptr);

    // 执行一条 patch 修正指令（通常用于写完 rawprogram 后修正 GPT CRC 等）。
    // 参数直接对应 patch*.xml 里的属性，具体数值计算是设备端 Firehose Loader
    // 自己完成的，主机只负责把 XML 指令转发过去。
    bool Patch(uint32_t sectorSize, uint64_t byteOffset, const std::string& filename,
               uint8_t physicalPartitionNumber, uint32_t sizeInBytes,
               uint64_t startSector, const std::string& value,
               const std::string& what = "", std::string* errorOut = nullptr);

    // 读取 GPT 分区表（读取每个 LUN 的第一个扇区块，一般前 6 个扇区含主GPT头+表项）
    bool ReadGpt(uint8_t physicalPartitionNumber, std::vector<uint8_t>* outData,
                 std::string* errorOut = nullptr);

    // 查询磁盘总容量（RawProgramParser::Resolve() 处理 "NUM_DISK_SECTORS-33." 这类
    // 表达式时需要这个值）。physicalPartitionNumber 一般传 0（主LUN）。
    bool GetStorageInfo(uint8_t physicalPartitionNumber, StorageInfo* outInfo,
                        std::string* errorOut = nullptr);

    // 复位设备，结束 EDL 会话，正常开机
    bool Reset(std::string* errorOut = nullptr);

private:
    zhi::usb::UsbDevice& device_;
    uint32_t sessionMaxPayload_ = 1048576;

    // 发送一段 XML 指令，读取 <response> 判定成功/失败
    bool SendXmlCommand(const std::string& xml, std::string* errorOut);

    // 从设备响应里解析 value="ACK"/"NACK"
    bool ReadAckResponse(std::string* errorOut, std::string* rawXmlOut = nullptr);
};

} // namespace zhi::protocol::edl
