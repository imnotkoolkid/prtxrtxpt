#pragma once
#include "resources.hpp"
#include "settings.hpp"
#include "world/trace_scene.hpp"
#include <functional>

class TraceRenderer
{
  public:
    using Submit = std::function<void(std::function<void(vk::CommandBuffer)>)>;
    TraceRenderer(Resources &, vk::ImageView atlas, vk::Sampler, Submit);
    void load(std::shared_ptr<World>);
    void presentation(vk::RenderPass);
    void record(vk::CommandBuffer, vk::Extent2D, const Camera &, const TraceSettings &, unsigned frame);
    void display(vk::CommandBuffer, vk::Extent2D, unsigned frame, bool denoise);
    void invalidate() { valid = false; }
    const World &world() const { return *scene.world; }

  private:
    struct Storage
    {
        Buffer buffer;
        size_t capacity{};
    };
    struct Film
    {
        Texture color, geometry, guide, specular;
        vk::DescriptorSet trace, filter[2], present[2];
    };
    Resources &gpu;
    vk::Device device;
    vk::ImageView atlas;
    vk::Sampler sampler;
    Submit submit;
    TraceScene scene;
    Storage grid, materials, lights;
    std::array<MappedBuffer, 2> uniforms;
    MappedBuffer staging;
    std::array<Film, 2> films;
    Texture scratch[2], specScratch[2];
    vk::UniqueDescriptorSetLayout traceSet, filterSet, presentSet;
    vk::UniquePipelineLayout traceLayout, filterLayout, presentLayout;
    vk::UniquePipeline tracePipe, filterPipe, presentPipe;
    vk::UniqueDescriptorPool pool;
    vk::Extent2D extent{};
    bool valid{}, setsDirty = true;
    Camera previous{};
    TraceSettings last;
    uint32_t sequence{};
    void upload(const TraceScene &);
    void resize(vk::Extent2D);
    void bind();
};
