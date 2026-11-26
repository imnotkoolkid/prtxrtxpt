#include "trace.hpp"
#include <bit>
#include <cmath>
#include <numbers>

namespace
{
constexpr uint32_t traceCode[] =
#include "trace.comp.inc"
    ;
constexpr uint32_t filterCode[] =
#include "filter.comp.inc"
    ;
constexpr uint32_t presentVS[] =
#include "present.vert.inc"
    ;
constexpr uint32_t presentFS[] =
#include "present.frag.inc"
    ;
using Type = vk::DescriptorType;
constexpr auto sampled = Type::eCombinedImageSampler, storage = Type::eStorageBuffer;
constexpr auto compute = vk::PipelineStageFlagBits::eComputeShader,
               fragment = vk::PipelineStageFlagBits::eFragmentShader;
constexpr auto read = vk::AccessFlagBits::eShaderRead, write = vk::AccessFlagBits::eShaderWrite;
constexpr auto general = vk::ImageLayout::eGeneral;
struct F4
{
    float x{}, y{}, z{}, w{};
};
struct Params
{
    F4 camera, previous, lens;
    std::array<int32_t, 4> limits, budgets, sampling;
    std::array<uint32_t, 4> sequence;
    F4 direction, sun, sky, emission;
};
static_assert(sizeof(Params) == 176);
struct FilterParams
{
    F4 camera;
    int stride;
};
F4 pose(Camera c) { return {c.pos.x, c.pos.y, c.pos.z, c.yaw}; }
F4 tint(int rgb, float intensity)
{
    auto channel = [&](int shift)
    {
        float v = float((rgb >> shift) & 255) / 255.f;
        return intensity * (v <= .04045f ? v / 12.92f : std::pow((v + .055f) / 1.055f, 2.4f));
    };
    return {channel(16), channel(8), channel(0), 0};
}
F4 solarColor(const TraceSettings &s)
{
    float t = s[Kelvin] * .01f;
    float r = t <= 66 ? 255.f : 329.6987f * std::pow(t - 60, -.133205f);
    float g = t <= 66 ? 99.4708f * std::log(t) - 161.1196f : 288.1222f * std::pow(t - 60, -.075515f);
    float b = t >= 66 ? 255.f : 138.5177f * std::log(t - 10) - 305.0448f;
    auto byte = [](float x) { return int(std::clamp(x, 0.f, 255.f)); };
    auto c = tint(byte(r) << 16 | byte(g) << 8 | byte(b), 1.f);
    auto a = tint(s[SunTint], s[Sun] * .035f);
    return {a.x * c.x, a.y * c.y, a.z * c.z, 0};
}
vk::UniqueDescriptorSetLayout setLayout(vk::Device d, std::initializer_list<Type> types,
                                        vk::ShaderStageFlagBits stage)
{
    std::vector<vk::DescriptorSetLayoutBinding> bindings;
    for (auto t : types)
        bindings.emplace_back(uint32_t(bindings.size()), t, 1, stage);
    return d.createDescriptorSetLayoutUnique(vk::DescriptorSetLayoutCreateInfo({}, bindings));
}
template <size_t N> vk::UniqueShaderModule module(vk::Device d, const uint32_t (&code)[N])
{
    return d.createShaderModuleUnique({{}, sizeof(code), code});
}
void dependency(vk::CommandBuffer cmd, vk::PipelineStageFlags src, vk::PipelineStageFlags dst,
                vk::AccessFlags before, vk::AccessFlags after)
{
    cmd.pipelineBarrier(src, dst, {}, vk::MemoryBarrier(before, after), {}, {});
}
} 

TraceRenderer::TraceRenderer(Resources &r, vk::ImageView atlasView, vk::Sampler nearest, Submit transfer)
    : gpu(r), device(r.device), atlas(atlasView), sampler(nearest), submit(std::move(transfer))
{
    auto cs = vk::ShaderStageFlagBits::eCompute;
    traceSet =
        setLayout(device,
                  {Type::eUniformBuffer, storage, storage, storage, sampled, sampled, sampled, sampled,
                   Type::eStorageImage, Type::eStorageImage, Type::eStorageImage, sampled, Type::eStorageImage},
                  cs);
    filterSet = setLayout(
        device,
        {sampled, sampled, sampled, sampled, Type::eStorageImage, sampled, Type::eStorageImage, sampled}, cs);
    presentSet = setLayout(device, {sampled, sampled, sampled}, vk::ShaderStageFlagBits::eFragment);
    traceLayout = device.createPipelineLayoutUnique(vk::PipelineLayoutCreateInfo({}, *traceSet));
    vk::PushConstantRange filterPush(cs, 0, sizeof(FilterParams));
    filterLayout = device.createPipelineLayoutUnique(vk::PipelineLayoutCreateInfo({}, *filterSet, filterPush));
    presentLayout = device.createPipelineLayoutUnique(vk::PipelineLayoutCreateInfo({}, *presentSet));
    auto make = [&](auto &code, vk::PipelineLayout layout, vk::PipelineCreateFlags flags)
    {
        auto shader = module(device, code);
        return device
            .createComputePipelineUnique(
                {}, vk::ComputePipelineCreateInfo(flags, {{}, cs, *shader, "main"}, layout))
            .value;
    };
    tracePipe = make(traceCode, *traceLayout, vk::PipelineCreateFlagBits::eDispatchBase);
    filterPipe = make(filterCode, *filterLayout, {});
    for (auto &u : uniforms)
        u.reserve(gpu, sizeof(Params), vk::BufferUsageFlagBits::eUniformBuffer);
}

void TraceRenderer::load(std::shared_ptr<World> w)
{
    TraceScene next;
    next.load(std::move(w));
    upload(next);
    scene = std::move(next);
}

void TraceRenderer::upload(const TraceScene &sourceScene)
{
    struct Copy
    {
        const std::byte *source;
        vk::Buffer dst;
        vk::BufferCopy region;
    };
    std::vector<Copy> copies;
    size_t stagingBytes = 0;
    struct Replacement
    {
        Storage *dst;
        Buffer next;
        size_t capacity;
    };
    std::vector<Replacement> replacements;
    auto collect = [&](Storage &dst, const auto &values)
    {
        size_t bytes = values.size() * sizeof(values.front());
        size_t required = std::max(bytes, sizeof(uint32_t));
        auto handle = dst.buffer.handle.get();
        if (required > dst.capacity)
        {
            size_t capacity = std::bit_ceil(required);
            auto fresh = gpu.buffer(
                capacity, vk::BufferUsageFlagBits::eStorageBuffer | vk::BufferUsageFlagBits::eTransferDst,
                vk::MemoryPropertyFlagBits::eDeviceLocal);
            handle = *fresh.handle;
            replacements.push_back({&dst, std::move(fresh), capacity});
        }
        if (bytes)
        {
            copies.push_back({reinterpret_cast<const std::byte *>(values.data()), handle, {stagingBytes, 0, bytes}});
            stagingBytes += bytes;
        }
    };
    collect(grid, sourceScene.grid);
    collect(materials, sourceScene.materials);
    collect(lights, sourceScene.lights);
    if (!copies.empty())
    {
        staging.reserve(gpu, std::bit_ceil(stagingBytes), vk::BufferUsageFlagBits::eTransferSrc);
        for (const auto &copy : copies)
            std::memcpy(static_cast<std::byte *>(staging.data) + copy.region.srcOffset,
                        copy.source, copy.region.size);
        submit(
            [&](vk::CommandBuffer cmd)
            {
                auto transfer = vk::PipelineStageFlagBits::eTransfer;
                dependency(cmd, compute, transfer, read, vk::AccessFlagBits::eTransferWrite);
                for (auto &c : copies)
                    cmd.copyBuffer(*staging.buffer.handle, c.dst, c.region);
                dependency(cmd, transfer, compute, vk::AccessFlagBits::eTransferWrite, read);
            });
    }
    for (auto &r : replacements)
    {
        std::swap(r.dst->buffer, r.next);
        r.dst->capacity = r.capacity;
        setsDirty = true;
    }
    valid = false;
}

void TraceRenderer::presentation(vk::RenderPass pass)
{
    auto vs = module(device, presentVS), fs = module(device, presentFS);
    vk::PipelineShaderStageCreateInfo stages[] = {{{}, vk::ShaderStageFlagBits::eVertex, *vs, "main"},
                                                  {{}, vk::ShaderStageFlagBits::eFragment, *fs, "main"}};
    vk::PipelineVertexInputStateCreateInfo input;
    vk::PipelineInputAssemblyStateCreateInfo assembly({}, vk::PrimitiveTopology::eTriangleList);
    vk::PipelineViewportStateCreateInfo viewport({}, 1, nullptr, 1, nullptr);
    vk::PipelineRasterizationStateCreateInfo raster({}, false, false, vk::PolygonMode::eFill, {},
                                                    vk::FrontFace::eCounterClockwise, false, 0, 0, 0, 1);
    vk::PipelineMultisampleStateCreateInfo samples({}, vk::SampleCountFlagBits::e1);
    vk::PipelineDepthStencilStateCreateInfo depth;
    vk::PipelineColorBlendAttachmentState rgba;
    rgba.colorWriteMask = vk::ColorComponentFlags(15);
    vk::PipelineColorBlendStateCreateInfo blend({}, false, vk::LogicOp::eCopy, rgba);
    vk::DynamicState states[] = {vk::DynamicState::eViewport, vk::DynamicState::eScissor};
    vk::PipelineDynamicStateCreateInfo dynamic({}, states);
    presentPipe =
        device
            .createGraphicsPipelineUnique(
                {}, vk::GraphicsPipelineCreateInfo({}, stages, &input, &assembly, nullptr, &viewport, &raster,
                                                   &samples, &depth, &blend, &dynamic, *presentLayout, pass))
            .value;
}

void TraceRenderer::resize(vk::Extent2D size)
{
    auto make = [&](vk::Format format)
    {
        return gpu.texture(size, format, vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eStorage,
                           vk::ImageAspectFlagBits::eColor);
    };

    if (extent.width)
        device.waitIdle();
    std::vector<vk::ImageMemoryBarrier> barriers;
    auto init = [&](Texture &tex, vk::Format format)
    {
        auto fresh = make(format);
        std::swap(tex, fresh);
        barriers.emplace_back(vk::AccessFlags{}, read | write, vk::ImageLayout::eUndefined, general,
                              VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, *tex.image,
                              vk::ImageSubresourceRange(vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1));
    };
    for (auto &film : films)
    {
        init(film.color, vk::Format::eR32G32B32A32Sfloat);
        init(film.specular, vk::Format::eR32G32B32A32Sfloat);
        init(film.geometry, vk::Format::eR32G32B32A32Sfloat);
        init(film.guide, vk::Format::eR16G16B16A16Sfloat);
    }
    for (auto &t : specScratch)
        init(t, vk::Format::eR16G16B16A16Sfloat);
    for (auto &t : scratch)
        init(t, vk::Format::eR16G16B16A16Sfloat);
    submit([&](vk::CommandBuffer cmd)
           { cmd.pipelineBarrier(vk::PipelineStageFlagBits::eTopOfPipe, compute, {}, {}, {}, barriers); });
    extent = size;
    valid = false;
    setsDirty = true;
}
void TraceRenderer::bind()
{
    vk::DescriptorPoolSize sizes[] = {
        {Type::eUniformBuffer, 2}, {storage, 6}, {sampled, 46}, {Type::eStorageImage, 16}};
    pool = device.createDescriptorPoolUnique(vk::DescriptorPoolCreateInfo({}, 10, sizes));
    auto allocate = [&](vk::DescriptorSetLayout layout)
    { return device.allocateDescriptorSets(vk::DescriptorSetAllocateInfo(*pool, layout))[0]; };
    auto buffer = [&](vk::DescriptorSet set, int n, vk::Buffer b, Type type)
    {
        vk::DescriptorBufferInfo info(b, 0, VK_WHOLE_SIZE);
        device.updateDescriptorSets(vk::WriteDescriptorSet(set, n, 0, type, {}, info), {});
    };
    auto image =
        [&](vk::DescriptorSet set, int n, vk::ImageView view, Type type, vk::ImageLayout layout = general)
    {
        vk::DescriptorImageInfo info(type == sampled ? sampler : vk::Sampler{}, view, layout);
        device.updateDescriptorSets(vk::WriteDescriptorSet(set, n, 0, type, info), {});
    };
    for (int i = 0; i < 2; ++i)
    {
        auto &f = films[i], &old = films[1 - i];
        f.trace = allocate(*traceSet);
        buffer(f.trace, 0, *uniforms[i].buffer.handle, Type::eUniformBuffer);
        buffer(f.trace, 1, *grid.buffer.handle, storage);
        buffer(f.trace, 2, *materials.buffer.handle, storage);
        buffer(f.trace, 3, *lights.buffer.handle, storage);
        image(f.trace, 4, atlas, sampled, vk::ImageLayout::eShaderReadOnlyOptimal);
        image(f.trace, 5, *old.color.view, sampled);
        image(f.trace, 6, *old.geometry.view, sampled);
        image(f.trace, 7, *old.guide.view, sampled);
        image(f.trace, 8, *f.color.view, Type::eStorageImage);
        image(f.trace, 9, *f.geometry.view, Type::eStorageImage);
        image(f.trace, 10, *f.guide.view, Type::eStorageImage);
        image(f.trace, 11, *old.specular.view, sampled);
        image(f.trace, 12, *f.specular.view, Type::eStorageImage);
        for (int j = 0; j < 2; ++j)
        {
            auto set = f.filter[j] = allocate(*filterSet);
            image(set, 0, j ? *scratch[0].view : *f.color.view, sampled);
            image(set, 1, *f.geometry.view, sampled);
            image(set, 2, *f.guide.view, sampled);
            image(set, 3, *f.color.view, sampled);
            image(set, 4, *scratch[j].view, Type::eStorageImage);
            image(set, 5, j ? *specScratch[0].view : *f.specular.view, sampled);
            image(set, 6, *specScratch[j].view, Type::eStorageImage);
            image(set, 7, *f.specular.view, sampled);
            set = f.present[j] = allocate(*presentSet);
            image(set, 0, j ? *scratch[1].view : *f.color.view, sampled);
            image(set, 1, *f.geometry.view, sampled);
            image(set, 2, j ? *specScratch[1].view : *f.specular.view, sampled);
        }
    }
    setsDirty = false;
}
void TraceRenderer::record(vk::CommandBuffer cmd, vk::Extent2D display, const Camera &cam,
                           const TraceSettings &s, unsigned frame)
{
    vk::Extent2D size(std::max(1u, display.width * s[Scale] / 100),
                      std::max(1u, display.height * s[Scale] / 100));
    if (extent != size)
        resize(size);
    if (setsDirty)
        bind();
    valid = valid && last.value == s.value;
    Params p{};
    p.camera = pose(cam);
    p.previous = pose(previous);
    p.lens = {cam.pitch, previous.pitch, float(display.width) / display.height, s[Indirect] * .01f};
    p.limits = {s[Spp], s[Bounces], s[Steps], s[Distance]};
    p.budgets = {s[Diffuse], s[Specular], s[Transmission], int(scene.lights.size())};
    int flags = s[Roulette] | (s[Mis] << 1) | (s[Adaptive] << 2) | (s[GI] << 3) | (s[Soft] << 4) |
                (s[Contact] << 5) | (s[SampleLights] << 6);
    p.sampling = {s[DirectSamples], s[LightSamples], flags, int(valid)};
    bool still = cam.pitch == previous.pitch && std::memcmp(&p.camera, &p.previous, sizeof(F4)) == 0;
    p.sequence = {sequence++, uint32_t(still), 0, 0};
    constexpr float rad = std::numbers::pi_v<float> / 180;
    float e = s[Elevation] * rad, a = s[Azimuth] * rad;
    p.direction = {std::cos(e) * std::sin(a), std::sin(e), std::cos(e) * std::cos(a), 0};
    p.sun = solarColor(s);
    if (e < 0)
        p.sun = {};
    p.sky = tint(s[SkyTint], s[Sky] * .01f);
    p.emission = tint(s[EmissionTint], s[Emission] * .01f);
    std::memcpy(uniforms[frame].data, &p, sizeof(p));
    auto &film = films[frame];
    dependency(cmd, compute | fragment, compute, read | write, read | write);
    auto dispatch = [&](vk::Pipeline pipe, vk::PipelineLayout layout, vk::DescriptorSet set)
    {
        cmd.bindPipeline(vk::PipelineBindPoint::eCompute, pipe);
        cmd.bindDescriptorSets(vk::PipelineBindPoint::eCompute, layout, 0, set, {});
        cmd.dispatch((size.width + 7) / 8, (size.height + 7) / 8, 1);
    };
    cmd.bindPipeline(vk::PipelineBindPoint::eCompute, *tracePipe);
    cmd.bindDescriptorSets(vk::PipelineBindPoint::eCompute, *traceLayout, 0, film.trace, {});
    uint32_t groupsX = (size.width + 7) / 8, groupsY = (size.height + 7) / 8;
    uint32_t raysPerGroup =
        64u * s[Spp] * s[Bounces] * (1u + 2 * s[DirectSamples] + (scene.lights.empty() ? 0 : s[LightSamples]));
    uint32_t groupBudget = std::max(1u, (1u << 24) / raysPerGroup);
    uint32_t tileX = std::min(groupsX, groupBudget), tileY = std::max(1u, groupBudget / tileX);
    for (uint32_t y = 0; y < groupsY; y += tileY)
        for (uint32_t x = 0; x < groupsX; x += tileX)
            cmd.dispatchBase(x, y, 0, std::min(tileX, groupsX - x), std::min(tileY, groupsY - y), 1);
    if (s[Denoise])
        for (int i = 0; i < 2; ++i)
        {
            dependency(cmd, compute, compute, write, read);
            FilterParams params{p.camera, 1 << i};
            cmd.pushConstants(*filterLayout, vk::ShaderStageFlagBits::eCompute, 0, sizeof(params), &params);
            dispatch(*filterPipe, *filterLayout, film.filter[i]);
        }
    dependency(cmd, compute, fragment, write, read);
    valid = true;
    previous = cam;
    last = s;
}
void TraceRenderer::display(vk::CommandBuffer cmd, vk::Extent2D size, unsigned frame, bool denoise)
{
    cmd.setViewport(0, vk::Viewport(0, 0, float(size.width), float(size.height), 0, 1));
    cmd.setScissor(0, vk::Rect2D({0, 0}, size));
    cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, *presentPipe);
    cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, *presentLayout, 0, films[frame].present[denoise],
                           {});
    cmd.draw(3, 1, 0, 0);
}
