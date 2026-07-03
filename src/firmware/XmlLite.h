#pragma once
// ============================================================================
// XmlLite —— 一个只干一件事的极简 XML 解析工具
//
// rawprogram*.xml / patch*.xml 这类 Firehose 配套文件有个特点：
// 所有需要的信息都在"自闭合标签的属性"里，没有嵌套结构、没有文本节点，
// 形如：
//     <program SECTOR_SIZE_IN_BYTES="4096" start_sector="33" .../>
//     <patch SECTOR_SIZE_IN_BYTES="4096" start_sector="..." value="0"/>
//
// 引入 pugixml 这类完整 XML 库当然更规范，但对这种场景其实是杀鸡用牛刀。
// 这里用 <regex> 做两级提取：先切出每个标签的完整文本，再从标签文本里
// 抠 key="value" 键值对。如果后续要处理更复杂的 XML（比如带嵌套/CDATA），
// 再换 pugixml 也不迟——但那时候 RawProgramParser/PatchParser 的调用方
// 接口不需要变，换的只是内部实现。
// ============================================================================

#include <string>
#include <vector>
#include <map>
#include <cstdint>

namespace zhi::firmware {

using XmlAttributes = std::map<std::string, std::string>;

class XmlLite {
public:
    // 从整份文件内容里找出所有 <tagName .../> 标签，逐个解析成属性表
    static std::vector<XmlAttributes> FindSelfClosingTags(const std::string& fileContent,
                                                            const std::string& tagName);

    // 便捷取值：属性不存在时返回 defaultValue
    static std::string Get(const XmlAttributes& attrs, const std::string& key,
                            const std::string& defaultValue = "");

    static uint64_t GetUInt64(const XmlAttributes& attrs, const std::string& key,
                               uint64_t defaultValue = 0);

    static uint32_t GetUInt32(const XmlAttributes& attrs, const std::string& key,
                               uint32_t defaultValue = 0);
};

} // namespace zhi::firmware
