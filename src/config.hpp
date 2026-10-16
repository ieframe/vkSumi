#pragma once

#include <array>
#include <memory>
#include <string>
#include <vector>

namespace vksumi
{
    // knobs.def is shared with color.frag
    struct alignas(16) Knobs
    {
        #define KNOB(name, value) float name = value;
        #include "knobs.def"
        #undef KNOB
    };

    enum class Daltonize : int { Off, Protan, Deutan, Tritan };

    struct Daltonization
    {
        Daltonize type     = Daltonize::Off;
        float     severity = 1.0f;
        float     strength = 1.0f;
    };

    struct alignas(16) CvdGPU
    {
        float columns[3][4] = {};
        float strength      = 0.0f;
    };
    static_assert(sizeof(CvdGPU) == 64, "CvdGPU layout drift");

    enum class CensorShape : int { Rect = 0, Ellipse = 1 };
    enum class CensorMode  : int { Pixelate = 0, Blur = 1, Fill = 2 };

    struct CensorRegion
    {
        bool        enabled  = true;
        float       x = 0.0f, y = 0.0f;
        float       w = 0.0f, h = 0.0f;
        CensorShape shape    = CensorShape::Rect;
        CensorMode  mode     = CensorMode::Pixelate;
        float       strength = 24.0f;
        float       feather  = 0.2f;
    };

    inline constexpr size_t kMaxCensor = VKSUMI_MAX_CENSOR;

    struct alignas(16) CensorGPU
    {
        float cx = 0, cy = 0, rx = 0, ry = 0;
        float strength = 0, feather = 0, shape = 0, mode = 0;
    };
    static_assert(sizeof(CensorGPU) == 32, "CensorGPU layout drift");

    struct GradeParams
    {
        Knobs     knobs;
        CvdGPU    cvd;
        CensorGPU region[kMaxCensor];
        float     srgb_output = 0.0f;
        float     compare     = 0.0f;
    };

    struct Config
    {
        Knobs       knobs;
        Daltonization daltonize;
        std::array<CensorRegion, kMaxCensor> censors;
        bool        enabled     = true;
        bool        compare     = false;
        std::string toggle_keys = "Shift_R+F9";
        std::vector<std::string> sources;
        std::vector<std::string> search_paths;
        std::string vksumi_config_file;
        bool        per_game_config_creation = true;
    };

    std::shared_ptr<const Config> loadConfig();
    std::shared_ptr<const Config> currentConfig();
    void                          setCurrentConfig(std::shared_ptr<const Config> next);

    GradeParams buildGradeParams(const Config& cfg);

    void startConfigWatcher();
    void stopConfigWatcher();

    void ensurePerGameConfig();
}
