// version_compare.h - 版本号比较（纯头文件，单测直接包含）
#pragma once

#include <cstdlib>
#include <string>

namespace rshell {

// "v1.2.10" 与 "1.9" 这类比较：按点分段，数字段比数值，非数字段比字符串。
// 返回值：candidate 比 current 新。
inline bool versionNewer(const std::string& candidate, const std::string& current) {
    const auto strip = [](const std::string& text) {
        std::size_t begin = 0;
        while (begin < text.size() && (text[begin] == 'v' || text[begin] == 'V')) ++begin;
        return text.substr(begin);
    };
    const std::string a = strip(candidate);
    const std::string b = strip(current);
    std::size_t ia = 0, ib = 0;
    while (ia < a.size() || ib < b.size()) {
        std::string sa, sb;
        while (ia < a.size() && a[ia] != '.') sa.push_back(a[ia++]);
        while (ib < b.size() && b[ib] != '.') sb.push_back(b[ib++]);
        if (ia < a.size()) ++ia;
        if (ib < b.size()) ++ib;
        const bool na = !sa.empty() && sa.find_first_not_of("0123456789") == std::string::npos;
        const bool nb = !sb.empty() && sb.find_first_not_of("0123456789") == std::string::npos;
        if (na && nb) {
            const long va = std::strtol(sa.c_str(), nullptr, 10);
            const long vb = std::strtol(sb.c_str(), nullptr, 10);
            if (va != vb) return va > vb;
        } else if (sa != sb) {
            return sa > sb;
        }
    }
    return false;
}

}  // namespace rshell
