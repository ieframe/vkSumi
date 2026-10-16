#pragma once

#include <array>

#include "config.hpp"

namespace vksumi
{
    std::array<float, 9> cvdMatrix(Daltonize type, float severity);
}
