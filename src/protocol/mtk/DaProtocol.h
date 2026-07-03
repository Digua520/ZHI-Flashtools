#pragma once
// ============================================================================
// DaProtocol —— BROM JumpDA 之后，DA (Download Agent) 自己的分区读写协议
//
// 老实话放在最前面：这一层的"协议机制"（命令+回显确认、分块传输、进度回调、
// 校验和收尾）是可以直接用的，跟 BromProtocol 是同一套设计思路，经得住
// 复用。但**具体的命令字节值（DaOpcodes 里的每一项）不同 DA 版本差异很大**
// ——市面上流传的 DA 二进制文件有很多个版本/分支，同一个命令在不同 DA 里
// 编号可能不一样。所以这里没有把 opcode 硬编码在函数体里，而是做成一张
// 可以在运行时/初始化时填入真实值的表（DaOpcodes），默认值是社区里比较
// 常见的一种，但我没有真实设备验证过，**接入前务必先核实**：
//   - 如果你有目标 DA 文件本身，用 IDA/Ghidra 简单静态分析一下命令分发表
//     （通常是个 switch-case 或跳转表）就能拿到准确值
//   - 或者抓一次真实 SP Flash Tool 线刷过程的 USB 包，直接对照
//
// 传输层的分块大小/超时值也是可以按需调的经验值，不是协议规定的固定值。
// ============================================================================

#include "../../usb/UsbDevice.h"
#include <string>
#include <vector>
#include <cstdint>
#include <functional>

namespace zhi::protocol::mtk {

// 命令字节表：默认值是社区常见写法，使用前请对照实际 DA 核实
struct DaOpcodes {
    uint8_t readPartition  = 0xD6;
    uint8_t writePartition = 0xD4;
    uint8_t formatPartition = 0xD9;
    uint8_t getPartitionInfo = 0xDC;
    uint8_t finish = 0xDF; // 结束会话，通常触发设备重启
};

struct DaPartitionTarget {
    std::string name;          // 分区名（对应 scatter 里的 partition_name）
    uint64_t    physicalAddr = 0; // 物理起始地址（对应 scatter 的 physical_start_addr）
    uint64_t    length = 0;       // 字节数（对应 scatter 的 partition_size 或实际镜像大小）
};

using ProgressCallback = std::function<void(uint64_t done, uint64_t total)>;

class DaProtocol {
public:
    explicit DaProtocol(zhi::usb::UsbDevice& device, DaOpcodes opcodes = DaOpcodes{})
        : device_(device), opcodes_(opcodes) {}

    // 写入一个分区（对应 scatter 里 is_download=true 的条目）
    bool WritePartition(const DaPartitionTarget& target, const uint8_t* data, size_t len,
                        ProgressCallback onProgress = nullptr, std::string* errorOut = nullptr);

    // 读取一个分区到内存（备份用途）
    bool ReadPartition(const DaPartitionTarget& target, std::vector<uint8_t>* outData,
                       ProgressCallback onProgress = nullptr, std::string* errorOut = nullptr);

    // 格式化一个分区（先擦除再置空，通常用于 userdata/cache 这类不需要写具体内容的分区）
    bool FormatPartition(const DaPartitionTarget& target, std::string* errorOut = nullptr);

    // 结束 DA 会话（通常会让设备正常开机）
    bool Finish(bool autoReboot, std::string* errorOut = nullptr);

    void SetChunkSize(size_t bytes) { chunkSize_ = bytes; }

private:
    zhi::usb::UsbDevice& device_;
    DaOpcodes opcodes_;
    size_t chunkSize_ = 4096; // 保守起见先用小块，联调稳定后可以调大提速

    bool SendCommandByte(uint8_t cmd, std::string* errorOut);
    static void WriteU32BE(std::vector<uint8_t>& buf, uint32_t val);
    static void WriteU64BE(std::vector<uint8_t>& buf, uint64_t val);
    static uint32_t ReadU32BE(const uint8_t* buf);

    // 16位异或校验和，跟 BromProtocol::SendDA 用的是同一套算法约定
    static uint16_t Checksum16(const uint8_t* data, size_t len);
};

} // namespace zhi::protocol::mtk
