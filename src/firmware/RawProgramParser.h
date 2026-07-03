#pragma once
// ============================================================================
// RawProgramParser —— 解析高通固件包里的 rawprogram*.xml
//
// 典型内容（每行一个 <program> 自闭合标签）：
//   <program SECTOR_SIZE_IN_BYTES="4096" file_sector_offset="0"
//     filename="xbl.elf" label="xbl_a" num_partition_sectors="10240"
//     physical_partition_number="0" size_in_KB="40960.0"
//     start_byte_hex="0x1800000" start_sector="1536" sparse="false"/>
//
// 坑点：start_sector / num_partition_sectors 不总是纯数字，末尾常见的备份GPT
// 分区会写成表达式，例如：
//   start_sector="NUM_DISK_SECTORS-33."
// 意思是"整块磁盘总扇区数 - 33"，因为这是备份GPT要写在磁盘最末尾。
// 这个表达式只有在设备回报了磁盘总容量（通常通过 <configure> 响应或
// 一次 storage info 查询获得）之后才能算出实际值，所以这里分两步：
//   1. Parse()        —— 只做文本解析，遇到表达式先原样存成字符串
//   2. Resolve(total) —— 拿到磁盘总扇区数后，把表达式换算成真实 ProgramEntry
// ============================================================================

#include "../protocol/edl/EdlTypes.h"
#include <string>
#include <vector>
#include <optional>

namespace zhi::firmware {

struct RawProgramRecord {
    std::string filename;
    std::string label;
    uint32_t    sectorSize = 4096;
    uint8_t     physicalPartitionNumber = 0;
    bool        sparse = false;

    // 原始字符串形式：可能是纯数字（"1536"），也可能是表达式（"NUM_DISK_SECTORS-33."）
    std::string startSectorRaw;
    std::string numSectorsRaw;
};

class RawProgramParser {
public:
    // 解析单个 rawprogram*.xml 文件内容（调用方自己读文件传字符串进来，
    // 方便单元测试时不依赖真实文件系统）
    static std::vector<RawProgramRecord> Parse(const std::string& xmlContent);

    // 把表达式换算成真实数值，生成可以直接喂给 FirehoseProtocol::Program() 的条目。
    // totalDiskSectors 传 0 表示"不知道磁盘总容量"，此时遇到表达式会解析失败并跳过，
    // 并通过 skippedOut 把被跳过的条目返回，方便 UI 层提示用户。
    static std::vector<protocol::edl::ProgramEntry> Resolve(
        const std::vector<RawProgramRecord>& records,
        uint64_t totalDiskSectors,
        std::vector<RawProgramRecord>* skippedOut = nullptr);

private:
    // 支持形如 "1536" / "NUM_DISK_SECTORS-33." / "NUM_DISK_SECTORS - 33" 的表达式求值
    static std::optional<uint64_t> EvalSectorExpr(const std::string& expr, uint64_t totalDiskSectors);
};

} // namespace zhi::firmware
