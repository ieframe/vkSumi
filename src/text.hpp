#pragma once

#include <string>
#include <string_view>

namespace vksumi
{
    inline std::string trim(std::string_view s)
    {
        const size_t a = s.find_first_not_of(" \t\r\n");
        if (a == std::string_view::npos) return {};
        const size_t b = s.find_last_not_of(" \t\r\n");
        return std::string(s.substr(a, b - a + 1));
    }
}
