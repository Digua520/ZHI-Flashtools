#include "RawProgramParser.h"
#include "XmlLite.h"
#include "../core/Logger.h"
#include <sstream>
#include <cctype>
#include <algorithm>

namespace zhi::firmware {

using zhi::Logger;

std::vector<RawProgramRecord> RawProgramParser::Parse(const std::string& xmlContent) {
    std::vector<RawProgramRecord> records;

    for (const auto& attrs : XmlLite::FindSelfClosingTags(xmlContent, "program")) {
        RawProgramRecord rec;
        rec.filename = XmlLite::Get(attrs, "filename");
        rec.label = XmlLite::Get(attrs, "label");
        rec.sectorSize = XmlLite::GetUInt32(attrs, "SECTOR_SIZE_IN_BYTES", 4096);
        rec.physicalPartitionNumber = static_cast<uint8_t>(XmlLite::GetUInt32(attrs, "physical_partition_number", 0));
        rec.sparse = (XmlLite::Get(attrs, "sparse") == "true");
        rec.startSectorRaw = XmlLite::Get(attrs, "start_sector", "0");
        rec.numSectorsRaw = XmlLite::Get(attrs, "num_partition_sectors", "0");

        // filename 为空的 <program> 条目通常是纯粹的分区表占位符（不写实际数据），
        // 保留下来但标记 label 供 UI 展示，Program() 阶段由调用方决定是否跳过
        records.push_back(std::move(rec));
    }

    Logger::Instance().Info("rawprogram 解析完成，共 " + std::to_string(records.size()) + " 条分区记录");
    return records;
}

std::optional<uint64_t> RawProgramParser::EvalSectorExpr(const std::string& exprIn, uint64_t totalDiskSectors) {
    std::string expr = exprIn;

    // 去掉常见的尾随句点（rawprogram 里数字常写成 "33." 这种浮点风格）
    if (!expr.empty() && expr.back() == '.') expr.pop_back();

    // 纯数字：直接返回
    bool isPureNumber = !expr.empty() &&
        std::all_of(expr.begin(), expr.end(), [](char c){ return isdigit((unsigned char)c) || c=='+' || c=='-'; });

    auto replacePos = expr.find("NUM_DISK_SECTORS");
    if (replacePos == std::string::npos) {
        if (isPureNumber) {
            try { return static_cast<uint64_t>(std::stoll(expr)); }
            catch (...) { return std::nullopt; }
        }
        return std::nullopt; // 出现了未知的符号名，无法求值
    }

    if (totalDiskSectors == 0) {
        return std::nullopt; // 需要磁盘总容量但调用方没提供
    }

    // 替换掉 NUM_DISK_SECTORS，剩下的应该是形如 "-33" 或 " - 33" 的简单加减表达式
    std::string remainder = expr.substr(replacePos + std::string("NUM_DISK_SECTORS").size());

    // 去空格
    remainder.erase(std::remove_if(remainder.begin(), remainder.end(), ::isspace), remainder.end());

    if (remainder.empty()) return totalDiskSectors;

    char op = remainder[0];
    if (op != '+' && op != '-') return std::nullopt;

    try {
        int64_t delta = std::stoll(remainder.substr(1));
        int64_t result = static_cast<int64_t>(totalDiskSectors) + (op == '+' ? delta : -delta);
        if (result < 0) return std::nullopt;
        return static_cast<uint64_t>(result);
    } catch (...) {
        return std::nullopt;
    }
}

std::vector<protocol::edl::ProgramEntry> RawProgramParser::Resolve(
    const std::vector<RawProgramRecord>& records,
    uint64_t totalDiskSectors,
    std::vector<RawProgramRecord>* skippedOut) {

    std::vector<protocol::edl::ProgramEntry> resolved;

    for (const auto& rec : records) {
        auto start = EvalSectorExpr(rec.startSectorRaw, totalDiskSectors);
        auto num = EvalSectorExpr(rec.numSectorsRaw, totalDiskSectors);

        if (!start || !num) {
            Logger::Instance().Warning("跳过分区 " + rec.label +
                "：无法求值 start_sector=\"" + rec.startSectorRaw +
                "\" 或 num_partition_sectors=\"" + rec.numSectorsRaw +
                "\"（可能需要先读取磁盘总容量）");
            if (skippedOut) skippedOut->push_back(rec);
            continue;
        }

        protocol::edl::ProgramEntry entry;
        entry.filename = rec.filename;
        entry.label = rec.label;
        entry.startSector = *start;
        entry.numSectors = *num;
        entry.sectorSize = rec.sectorSize;
        entry.physicalPartitionNumber = rec.physicalPartitionNumber;

        resolved.push_back(std::move(entry));
    }

    Logger::Instance().Info("rawprogram 换算完成：" + std::to_string(resolved.size()) +
        " 条可写入，" + std::to_string(skippedOut ? skippedOut->size() : 0) + " 条被跳过");

    return resolved;
}

} // namespace zhi::firmware
