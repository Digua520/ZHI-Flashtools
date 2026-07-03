#pragma once
// ============================================================================
// EdlTypes —— ProgramEntry / StorageInfo 这几个纯数据结构单独放一个文件
//
// 之前这两个结构体定义在 FirehoseProtocol.h 里，结果 firmware/RawProgramParser.h
// 只是想解析个 rawprogram.xml 文件，却因为 #include FirehoseProtocol.h 被迫
// 连带引入 UsbDevice.h -> windows.h/winusb.h 这一整条 Windows 专属依赖链。
// 这意味着"解析一个XML文件"这种纯逻辑操作，在非Windows环境下（比如写单元测试、
// 或者用 g++ 在 Linux 上先做语法检查）根本编不过——这个问题是在给 Python 版本
// 做同样的解耦时才发现的，C++版一直没注意到，现在一并修掉。
//
// 拆出来之后，firmware/ 整个目录不再依赖任何 Windows 头文件，可以用普通的
// g++/clang++ 在任何平台上编译测试，这对写单元测试、CI 里做静态检查都有意义。
// ============================================================================

#include <string>
#include <cstdint>

namespace zhi::protocol::edl {

struct ProgramEntry {
    std::string filename;
    std::string label;
    uint64_t    startSector = 0;
    uint64_t    numSectors  = 0;
    uint32_t    sectorSize  = 4096;
    uint8_t     physicalPartitionNumber = 0;
};

struct StorageInfo {
    uint64_t totalBlocks = 0;
    uint32_t blockSize = 0;
    std::string rawJson;
};

} // namespace zhi::protocol::edl
