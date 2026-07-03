#include "XmlLite.h"
#include <regex>
#include <cstdlib>

namespace zhi::firmware {

std::vector<XmlAttributes> XmlLite::FindSelfClosingTags(const std::string& fileContent,
                                                          const std::string& tagName) {
    std::vector<XmlAttributes> results;

    // 第一级：切出 <tagName ... /> 或 <tagName ... > 的完整标签文本
    // （容忍标签内换行、容忍标签用 "/>" 或 ">" 结尾两种写法）
    std::regex tagRegex("<" + tagName + R"(\b([^>]*)/?>)");
    auto begin = std::sregex_iterator(fileContent.begin(), fileContent.end(), tagRegex);
    auto end = std::sregex_iterator();

    // 第二级：从标签内部文本里抠 key="value"（value 允许为空字符串）
    // 注意：这里必须用自定义分隔符 R"RGX(...)RGX"，不能用默认的 R"(...)"——
    // 默认分隔符遇到模式串内部的 )" 就会提前截断（模式里 [^"]*)" 这一段
    // 刚好撞上默认终止符），之前这里就是这个bug，导致整个文件编译不过。
    std::regex attrRegex(R"RGX((\w+)\s*=\s*"([^"]*)")RGX");

    for (auto it = begin; it != end; ++it) {
        std::string inner = (*it)[1].str();
        XmlAttributes attrs;

        auto attrBegin = std::sregex_iterator(inner.begin(), inner.end(), attrRegex);
        auto attrEnd = std::sregex_iterator();
        for (auto ait = attrBegin; ait != attrEnd; ++ait) {
            attrs[(*ait)[1].str()] = (*ait)[2].str();
        }
        results.push_back(std::move(attrs));
    }
    return results;
}

std::string XmlLite::Get(const XmlAttributes& attrs, const std::string& key,
                          const std::string& defaultValue) {
    auto it = attrs.find(key);
    return it != attrs.end() ? it->second : defaultValue;
}

uint64_t XmlLite::GetUInt64(const XmlAttributes& attrs, const std::string& key, uint64_t defaultValue) {
    auto it = attrs.find(key);
    if (it == attrs.end() || it->second.empty()) return defaultValue;
    // rawprogram 里的数字属性有时带小数点结尾（如 "33." ），strtoull 会自然截断到整数部分
    return strtoull(it->second.c_str(), nullptr, 10);
}

uint32_t XmlLite::GetUInt32(const XmlAttributes& attrs, const std::string& key, uint32_t defaultValue) {
    return static_cast<uint32_t>(GetUInt64(attrs, key, defaultValue));
}

} // namespace zhi::firmware
