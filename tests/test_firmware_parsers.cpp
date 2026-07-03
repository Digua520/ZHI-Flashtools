#include "firmware/RawProgramParser.h"
#include "firmware/PatchParser.h"
#include "core/Logger.h"
#include <iostream>
#include <cassert>

using namespace zhi::firmware;

int main() {
    zhi::Logger::Instance().SetSink([](zhi::LogLevel, const std::string& msg) {
        std::cout << "[log] " << msg << "\n";
    });

    std::string rawprogramXml = R"(<?xml version="1.0" ?>
<data>
<program SECTOR_SIZE_IN_BYTES="4096" filename="xbl.elf" label="xbl_a"
    num_partition_sectors="10240" physical_partition_number="0" start_sector="1536"/>
<program SECTOR_SIZE_IN_BYTES="4096" filename="" label="backup_gpt"
    num_partition_sectors="33" physical_partition_number="0"
    start_sector="NUM_DISK_SECTORS-33."/>
</data>)";

    auto records = RawProgramParser::Parse(rawprogramXml);
    std::cout << "rawprogram解析条目数: " << records.size() << "\n";
    assert(records.size() == 2);

    std::vector<RawProgramRecord> skipped;
    auto entries = RawProgramParser::Resolve(records, 0, &skipped);
    std::cout << "不给容量时: 可写入=" << entries.size() << " 跳过=" << skipped.size() << "\n";
    assert(entries.size() == 1 && skipped.size() == 1);

    skipped.clear();
    auto entries2 = RawProgramParser::Resolve(records, 61071360, &skipped);
    std::cout << "给容量后: 可写入=" << entries2.size() << " 跳过=" << skipped.size() << "\n";
    assert(entries2.size() == 2 && skipped.empty());
    assert(entries2[1].startSector == 61071360 - 33);
    std::cout << "backup_gpt start_sector = " << entries2[1].startSector << " (预期 " << (61071360-33) << ")\n";

    std::string patchXml = R"(<?xml version="1.0" ?>
<data>
<patch SECTOR_SIZE_IN_BYTES="4096" byte_offset="0" filename="DISK"
    physical_partition_number="0" size_in_bytes="4" start_sector="1"
    value="0" what="Primary GPT Header CRC32"/>
</data>)";
    auto patches = PatchParser::Parse(patchXml);
    std::cout << "patch解析条目数: " << patches.size() << " - " << patches[0].what << "\n";
    assert(patches.size() == 1);

    std::cout << "\n全部通过\n";
    return 0;
}
