#pragma once
#include <algorithm>
#include <array>
#include <cstdint>

enum Parameter
{
    Enabled,
    Spp,
    Bounces,
    Diffuse,
    Specular,
    Transmission,
    Distance,
    Steps,
    Scale,
    Roulette,
    Mis,
    Adaptive,
    DirectSamples,
    LightSamples,
    GI,
    Indirect,
    Sun,
    Sky,
    Emission,
    Kelvin,
    Azimuth,
    Elevation,
    SunTint,
    SkyTint,
    EmissionTint,
    Soft,
    Contact,
    SampleLights,
    Denoise,
    ParameterCount
};
struct Control
{
    const wchar_t *label;
    int low, high, initial, group;
};
inline constexpr std::array<Control, ParameterCount> controls{{{L"Path tracing", 0, 1, 1, 0},
                                                               {L"Samples / pixel / frame", 1, 64, 1, 0},
                                                               {L"Total bounces", 1, 16, 5, 0},
                                                               {L"Diffuse bounces", 0, 16, 4, 0},
                                                               {L"Specular bounces", 0, 16, 5, 0},
                                                               {L"Transmission bounces", 0, 16, 5, 0},
                                                               {L"Ray distance (blocks)", 16, 512, 512, 0},
                                                               {L"Traversal steps", 64, 2048, 512, 0},
                                                               {L"Internal resolution (%)", 25, 100, 50, 0},
                                                               {L"Russian roulette", 0, 1, 1, 0},
                                                               {L"Multiple importance sampling", 0, 1, 1, 0},
                                                               {L"Adaptive sampling", 0, 1, 1, 0},
                                                               {L"Direct samples", 1, 8, 1, 0},
                                                               {L"Emissive samples", 1, 8, 1, 0},
                                                               {L"Global illumination", 0, 1, 1, 1},
                                                               {L"Indirect intensity (%)", 0, 200, 100, 1},
                                                               {L"Sun intensity (%)", 0, 200, 100, 1},
                                                               {L"Sky intensity (%)", 0, 200, 100, 1},
                                                               {L"Emission intensity (%)", 0, 200, 100, 1},
                                                               {L"Sun temperature (K)", 2000, 12000, 5800, 1},
                                                               {L"Sun azimuth (degrees)", 0, 359, 35, 1},
                                                               {L"Sun elevation (degrees)", -90, 90, 42, 1},
                                                               {L"Sun tint", 0, 0xffffff, 0xffffff, 1},
                                                               {L"Sky tint", 0, 0xffffff, 0xffffff, 1},
                                                               {L"Emission tint", 0, 0xffffff, 0xffffff, 1},
                                                               {L"Soft shadows", 0, 1, 1, 1},
                                                               {L"Contact shadows", 0, 1, 1, 1},
                                                               {L"Sample emissive blocks", 0, 1, 1, 1},
                                                               {L"Denoise", 0, 1, 1, 0}}};
struct TraceSettings
{
    std::array<int, ParameterCount> value{};
    TraceSettings()
    {
        for (int i = 0; i < ParameterCount; ++i)
            value[i] = controls[i].initial;
    }
    int operator[](Parameter p) const { return value[p]; }
    void set(Parameter p, int v) { value[p] = std::clamp(v, controls[p].low, controls[p].high); }
    void preset(int quality)
    {
        static constexpr int budgets[][5] = {
            {35, 1, 3, 256, 1}, {50, 1, 5, 512, 1}, {75, 2, 8, 1024, 1}, {100, 4, 12, 2048, 2}};
        auto &b = budgets[std::clamp(quality, 0, 3)];
        set(Scale, b[0]);
        set(Spp, b[1]);
        set(Bounces, b[2]);
        set(Diffuse, b[2]);
        set(Specular, b[2]);
        set(Transmission, b[2]);
        set(Steps, b[3]);
        set(DirectSamples, b[4]);
        set(LightSamples, b[4]);
    }
};
