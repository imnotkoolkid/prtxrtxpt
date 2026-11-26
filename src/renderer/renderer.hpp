#pragma once
#include "world/map.hpp"
#include <memory>
#include "settings.hpp"
#include <string>
#include <windows.h>
class Renderer
{
  public:
    explicit Renderer(HWND);
    ~Renderer();
    void upload(Map &);
    TraceSettings settings;
    bool draw(Camera const &, bool measure);
    void setFullScreen(bool fullscreen, bool acquire);
    double cpuMilliseconds() const;
    double gpuMilliseconds() const;
    const std::string &adapterName() const;

  private:
    struct State;
    std::unique_ptr<State> s;
};
