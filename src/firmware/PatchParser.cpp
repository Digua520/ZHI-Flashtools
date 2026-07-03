#include "PatchParser.h"
#include "XmlLite.h"
#include "../core/Logger.h"

namespace zhi::firmware {

using zhi::Logger;

std::vector<PatchRecord> PatchParser::Parse(const std::string& xmlContent) {
    std::vector<PatchRecord> records;

    for (const auto& attrs : XmlLite::FindSelfClosingTags(xmlContent, "patch")) {
        PatchRecord rec;
        rec.sectorSize = XmlLite::GetUInt32(attrs, "SECTOR_SIZE_IN_BYTES", 4096);
        rec.byteOffset = XmlLite::GetUInt64(attrs, "byte_offset", 0);
        rec.filename = XmlLite::Get(attrs, "filename", "DISK");
        rec.physicalPartitionNumber = static_cast<uint8_t>(XmlLite::GetUInt32(attrs, "physical_partition_number", 0));
        rec.sizeInBytes = XmlLite::GetUInt32(attrs, "size_in_bytes", 4);
        rec.startSectorRaw = XmlLite::Get(attrs, "start_sector", "0");
        rec.value = XmlLite::Get(attrs, "value", "0");
        rec.what = XmlLite::Get(attrs, "what");

        records.push_back(std::move(rec));
    }

    Logger::Instance().Info("patch 解析完成，共 " + std::to_string(records.size()) + " 条修正记录");
    return records;
}

} // namespace zhi::firmware
