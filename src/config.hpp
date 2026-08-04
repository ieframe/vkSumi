#pragma once

#include <array>
#include <memory>
#include <string>
#include <vector>

namespace vksumi
{
    // push-constant block, matches color.frag's Knobs layout exactly.
    // 0 = no change for every knob. positive = more / brighter.
    struct alignas(16) Knobs
    {
        float brightness = 0.0f;   // [-1 .. 1]
        float contrast   = 0.0f;   // [-1 .. 1]
        float exposure   = 0.0f;   // EV stops, mul = 2^value, [-3 .. 3]
        float saturation = 0.0f;   // -1 = grayscale, [-1 .. 1]

        float vibrance   = 0.0f;   // smarter saturation
        float hue_deg    = 0.0f;   // [-180 .. 180]
        float gamma      = 0.0f;   // + brightens midtones
        float temperature = 0.0f;  // warm(+) cool(-)

        float tint       = 0.0f;   // magenta(+) green(-)
        float red_gain   = 0.0f;
        float green_gain = 0.0f;
        float blue_gain  = 0.0f;

        float shadows    = 0.0f;
        float midtones   = 0.0f;
        float highlights = 0.0f;
        float _pad       = 0.0f;
    };

    static_assert(sizeof(Knobs) == 64, "Knobs layout drift");

    enum class CensorShape : int { Rect = 0, Ellipse = 1 };
    enum class CensorMode  : int { Pixelate = 0, Blur = 1, Fill = 2 };

    struct CensorRegion
    {
        bool  enabled  = false;
        float cx = 0.5f, cy = 0.5f;
        float rx = 0.05f, ry = 0.03f;
        int   shape    = static_cast<int>(CensorShape::Rect);
        int   mode     = static_cast<int>(CensorMode::Pixelate);
        float strength = 24.0f;
        float feather  = 0.2f;
    };

    inline constexpr size_t kMaxCensor = 2;

    struct alignas(16) CensorGPU
    {
        float cx = 0, cy = 0, rx = 0, ry = 0;
        float strength = 0, feather = 0, shape = 0, mode = 0;
    };
    static_assert(sizeof(CensorGPU) == 32, "CensorGPU layout drift");

    struct FragPush
    {
        Knobs     knobs;
        CensorGPU region[kMaxCensor];
    };
    static_assert(sizeof(FragPush) == 128, "FragPush over push-constant budget");

    struct Config
    {
        Knobs       knobs;
        std::array<CensorRegion, kMaxCensor> censors;
        bool        enabled     = true;
        std::string toggle_keys = "Shift_R+F9";
        std::vector<std::string> sources;
        std::string vksumi_config_file;
        bool        per_game_config_creation = true;
    };

    std::shared_ptr<const Config> loadConfig();
    std::shared_ptr<const Config> currentConfig();
    void                          setCurrentConfig(std::shared_ptr<const Config> next);

    FragPush buildPush(const Config& cfg);

    void startConfigWatcher();
    void stopConfigWatcher();

    void ensurePerGameConfig();
}
