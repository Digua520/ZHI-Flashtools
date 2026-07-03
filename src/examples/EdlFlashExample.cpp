// ============================================================================
// EdlFlashExample —— 演示如何把 usb / Sahara / Firehose / RawProgramParser
// 这几层拼起来，走完一次"识别设备 -> 握手 -> 查询磁盘容量 -> 按 rawprogram
// 写分区 -> 打patch -> 复位"的完整流程。这不是生产代码，是给 UI 层"开始
// 全盘烧录"按钮写业务逻辑时的参考骨架，实际项目里这部分要接 UI 的进度条/
// 取消按钮/异常弹窗。
//
// 磁盘总容量通过 FirehoseProtocol::GetStorageInfo() 查询获得，用来换算
// rawprogram 里 "NUM_DISK_SECTORS-33." 这类表达式（备份GPT分区的位置）。
// 注意 GetStorageInfo 里 total_blocks/block_size 字段名是按最常见的情况写的，
// 不同芯片平台字段可能不同，如果解析失败请打印 StorageInfo::rawJson 核对。
// ============================================================================

#include "../usb/UsbDevice.h"
#include "../protocol/edl/SaharaProtocol.h"
#include "../protocol/edl/FirehoseProtocol.h"
#include "../firmware/RawProgramParser.h"
#include "../firmware/PatchParser.h"
#include "../core/Logger.h"

#include <fstream>
#include <sstream>

namespace zhi::examples {

using namespace zhi;
using namespace zhi::protocol::edl;
using namespace zhi::firmware;

static std::string ReadFileToString(const std::wstring& path) {
    std::ifstream f(path, std::ios::binary);
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

bool RunFullEdlFlash(const std::wstring& devicePath,
                      const std::wstring& loaderPath,
                      const std::wstring& rawprogramPath,
                      const std::wstring& patchPath) {
    usb::UsbDevice device;
    std::string err;

    if (!device.Open(devicePath, &err)) {
        Logger::Instance().Error("打开 9008 设备失败: " + err);
        return false;
    }

    // 第一步：Sahara 握手，把 Firehose Loader 送进设备并启动
    SaharaProtocol sahara(device);
    if (!sahara.PerformHandshake(loaderPath, &err)) {
        Logger::Instance().Error("Sahara 握手失败: " + err);
        return false;
    }

    // 第二步：Firehose Configure
    FirehoseProtocol firehose(device);
    if (!firehose.Configure(1048576, &err)) {
        Logger::Instance().Error("Firehose Configure 失败: " + err);
        return false;
    }

    // 第三步：查询磁盘总容量，供下一步换算 "NUM_DISK_SECTORS-33." 这类表达式用
    StorageInfo storageInfo;
    uint64_t totalDiskSectors = 0;
    if (firehose.GetStorageInfo(0, &storageInfo, &err)) {
        totalDiskSectors = storageInfo.totalBlocks;
    } else {
        Logger::Instance().Warning("未能获取磁盘总容量(" + err +
            ")，末尾的备份GPT分区将被跳过，可继续烧录其余分区");
    }

    // 第四步：解析 rawprogram，换算表达式，逐个分区写入
    std::string rawXml = ReadFileToString(rawprogramPath);
    auto records = RawProgramParser::Parse(rawXml);

    std::vector<RawProgramRecord> skipped;
    auto entries = RawProgramParser::Resolve(records, totalDiskSectors, &skipped);

    if (!skipped.empty()) {
        Logger::Instance().Warning(std::to_string(skipped.size()) +
            " 个分区因表达式无法求值被跳过，请检查是否需要提供磁盘总容量");
    }

    for (const auto& entry : entries) {
        if (entry.filename.empty()) {
            // 空 filename 的条目一般是纯分区表占位符，不需要真的写数据
            continue;
        }

        std::ifstream imgFile(entry.filename, std::ios::binary | std::ios::ate);
        if (!imgFile) {
            Logger::Instance().Error("找不到镜像文件: " + entry.filename + "，跳过分区 " + entry.label);
            continue;
        }
        size_t size = static_cast<size_t>(imgFile.tellg());
        imgFile.seekg(0);
        std::vector<uint8_t> data(size);
        imgFile.read(reinterpret_cast<char*>(data.data()), size);

        bool ok = firehose.Program(entry, data.data(), data.size(),
            [&](uint64_t sent, uint64_t total) {
                // 这里挂 UI 进度条回调
            }, &err);

        if (!ok) {
            Logger::Instance().Error("分区 " + entry.label + " 写入失败: " + err);
            return false; // 生产代码里这里应该让用户决定"中止"还是"跳过继续"
        }
    }

    // 第四步：应用 patch（GPT CRC 修正等）
    std::string patchXml = ReadFileToString(patchPath);
    auto patches = PatchParser::Parse(patchXml);

    for (const auto& p : patches) {
        // start_sector 同样可能是表达式，复用 RawProgramParser 的求值逻辑会更干净，
        // 这里为了示例简洁直接内联处理最常见的纯数字情况
        uint64_t startSector = strtoull(p.startSectorRaw.c_str(), nullptr, 10);

        firehose.Patch(p.sectorSize, p.byteOffset, p.filename, p.physicalPartitionNumber,
                        p.sizeInBytes, startSector, p.value, p.what, &err);
    }

    // 第五步：复位设备，正常开机
    firehose.Reset(&err);

    Logger::Instance().Success("全盘烧录流程结束");
    return true;
}

} // namespace zhi::examples
