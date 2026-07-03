#pragma once
// ============================================================================
// PatchParser —— 解析 patch*.xml
//
// patch 文件通常在 rawprogram 全部写完之后执行，典型用途是修正主/备份 GPT
// 的 CRC32 校验值（写分区表时先写占位值，写完实际分区再统一修正）。
// 典型条目：
//   <patch SECTOR_SIZE_IN_BYTES="4096" byte_offset="0" filename="DISK"
//     physical_partition_number="0" size_in_bytes="4"
//     start_sector="NUM_DISK_SECTORS-33." value="0"
//     what="Backup GPT Header CRC32"/>
//
// 关键点：patch 命令是**发给设备执行的**，不需要主机计算 CRC 之类的值——
// Firehose Loader 收到 <patch> 指令后自己读取/计算/回写，主机只是原样转发
// XML 属性。所以这里的解析器只负责"读文件 -> 结构化"，实际执行由
// FirehoseProtocol::Patch() 逐条发送。
// ============================================================================

#include <string>
#include <vector>
#include <cstdint>

namespace zhi::firmware {

struct PatchRecord {
    uint32_t    sectorSize = 4096;
    uint64_t    byteOffset = 0;
    std::string filename;      // 通常是 "DISK"，表示直接对磁盘操作而非某个文件
    uint8_t     physicalPartitionNumber = 0;
    uint32_t    sizeInBytes = 4;
    std::string startSectorRaw; // 同 rawprogram，可能含 NUM_DISK_SECTORS 表达式
    std::string value;
    std::string what;          // 人类可读描述，仅用于日志展示
};

class PatchParser {
public:
    static std::vector<PatchRecord> Parse(const std::string& xmlContent);
};

} // namespace zhi::firmware
