#include "firmware/ScatterParser.h"
#include "core/Logger.h"
#include <iostream>
#include <cassert>

using namespace zhi::firmware;

int main() {
    zhi::Logger::Instance().SetSink([](zhi::LogLevel, const std::string& msg) {
        std::cout << "[log] " << msg << "\n";
    });

    std::string scatterTxt = R"(
- partition_index: SYS12
  partition_name: boot
  file_name: boot.img
  is_download: true
  type: NORMAL_ROM
  linear_start_addr: 0x8A00000
  physical_start_addr: 0x8A00000
  partition_size: 0x4000000
  region: EMMC_USER
  storage: HW_STORAGE_EMMC
  boundary_check: true
  is_reserved: false
  operation_type: UPDATE
  reserve: 0x0

- partition_index: SYS13
  partition_name: userdata
  file_name:
  is_download: false
  type: NORMAL_ROM
  linear_start_addr: 0x10000000
  physical_start_addr: 0x10000000
  partition_size: 0x100000000
  storage: HW_STORAGE_EMMC
  is_reserved: false
  operation_type: UPDATE
)";

    auto partitions = ScatterParser::Parse(scatterTxt);
    std::cout << "scatter解析分区数: " << partitions.size() << "\n";
    assert(partitions.size() == 2);
    assert(partitions[0].partitionName == "boot");
    assert(partitions[0].isDownload == true);
    assert(partitions[0].physicalStartAddr == 0x8A00000);
    assert(partitions[1].partitionName == "userdata");
    assert(partitions[1].isDownload == false);

    std::cout << "boot: physicalStartAddr=0x" << std::hex << partitions[0].physicalStartAddr
              << " partitionSize=0x" << partitions[0].partitionSize << std::dec << "\n";
    std::cout << "\n全部通过\n";
    return 0;
}
