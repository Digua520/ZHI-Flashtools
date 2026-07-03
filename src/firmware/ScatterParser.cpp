#include "ScatterParser.h"
#include "../core/Logger.h"
#include <sstream>
#include <algorithm>
#include <cctype>

namespace zhi::firmware {

using zhi::Logger;

namespace {

std::string Trim(const std::string& s) {
    size_t start = s.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) return "";
    size_t end = s.find_last_not_of(" \t\r\n");
    return s.substr(start, end - start + 1);
}

// 从形如 "key: value" 的行里拆出 key/value（scatter 是 YAML，冒号后一个空格是惯例，
// 但保险起见还是按第一个冒号切分，再各自 trim）
bool SplitKeyValue(const std::string& line, std::string* key, std::string* value) {
    auto pos = line.find(':');
    if (pos == std::string::npos) return false;
    *key = Trim(line.substr(0, pos));
    *value = Trim(line.substr(pos + 1));
    return true;
}

uint64_t ParseNumber(const std::string& value) {
    if (value.empty()) return 0;
    // scatter 里的地址/大小几乎都是 0x 开头的十六进制
    return strtoull(value.c_str(), nullptr, 0);
}

bool ParseBool(const std::string& value) {
    return value == "true";
}

} // namespace

std::vector<ScatterPartition> ScatterParser::Parse(const std::string& scatterContent) {
    std::vector<ScatterPartition> result;

    std::istringstream stream(scatterContent);
    std::string line;
    bool inPartitionBlock = false;
    ScatterPartition current;

    auto flush = [&]() {
        if (inPartitionBlock && !current.partitionName.empty()) {
            result.push_back(current);
        }
    };

    while (std::getline(stream, line)) {
        std::string trimmed = Trim(line);
        if (trimmed.empty() || trimmed[0] == '#') continue;

        // 每个分区条目以 "- partition_index:" 开头，标志着新分区块的开始
        if (trimmed.rfind("- partition_index:", 0) == 0 ||
            trimmed.rfind("-partition_index:", 0) == 0) {
            flush();
            current = ScatterPartition{};
            inPartitionBlock = true;
            continue;
        }

        if (!inPartitionBlock) continue;

        std::string key, value;
        if (!SplitKeyValue(trimmed, &key, &value)) continue;

        if (key == "partition_name") current.partitionName = value;
        else if (key == "file_name") current.fileName = (value == "" ? "" : value);
        else if (key == "is_download") current.isDownload = ParseBool(value);
        else if (key == "linear_start_addr") current.linearStartAddr = ParseNumber(value);
        else if (key == "physical_start_addr") current.physicalStartAddr = ParseNumber(value);
        else if (key == "partition_size") current.partitionSize = ParseNumber(value);
        else if (key == "storage") current.storage = value;
        else if (key == "is_reserved") current.isReserved = ParseBool(value);
        else if (key == "operation_type") current.operationType = value;
    }
    flush(); // 文件里最后一个分区块没有下一个 "- partition_index:" 触发收尾，要手动flush一次

    Logger::Instance().Info("Scatter 解析完成，共 " + std::to_string(result.size()) + " 个分区");
    return result;
}

} // namespace zhi::firmware
