#pragma once
// ============================================================================
// ScatterParser —— 解析联发科线刷包里的 MTxxxx_Android_scatter.txt
//
// 格式说明：Scatter 文件是 YAML，不是 XML，每个分区是一个列表项，形如：
//
//   - partition_index: SYS12
//     partition_name: boot
//     file_name: boot.img
//     is_download: true
//     type: NORMAL_ROM
//     linear_start_addr: 0x8A00000
//     physical_start_addr: 0x8A00000
//     partition_size: 0x4000000
//     region: EMMC_USER
//     storage: HW_STORAGE_EMMC
//     boundary_check: true
//     is_reserved: false
//     operation_type: UPDATE
//     reserve: 0x0
//
// 本解析器不引入完整 YAML 库（对这种扁平结构没必要），按"以 partition_index
// 开头的行分段，段内逐行按 key: value 提取"的方式手工解析，足够稳定。
// ============================================================================

#include <string>
#include <vector>
#include <cstdint>

namespace zhi::firmware {

struct ScatterPartition {
    std::string partitionName;    // partition_name，如 "boot"
    std::string fileName;         // file_name，要写入的镜像文件，可能为空（不下载的占位分区）
    bool        isDownload = false;
    uint64_t    linearStartAddr = 0;
    uint64_t    physicalStartAddr = 0;
    uint64_t    partitionSize = 0;
    std::string storage;          // HW_STORAGE_EMMC / HW_STORAGE_NAND 等
    bool        isReserved = false;
    std::string operationType;    // UPDATE / BOOTLOADERS 等，格式化时通常只处理 UPDATE 类型
};

class ScatterParser {
public:
    static std::vector<ScatterPartition> Parse(const std::string& scatterContent);
};

} // namespace zhi::firmware
