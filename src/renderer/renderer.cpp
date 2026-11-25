#include "renderer.hpp"
#include <DirectXMath.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <stdexcept>
#include <vulkan/vulkan.hpp>

namespace
{
using namespace DirectX;
constexpr uint32_t vertexCode[] =
#include "map.vert.inc"
    ;
constexpr uint32_t fragmentCode[] =
#include "map.frag.inc"
    ;

struct Buffer  { vk::UniqueDeviceMemory memory; vk::UniqueBuffer handle; };
struct Texture { vk::UniqueDeviceMemory memory; vk::UniqueImage image; vk::UniqueImageView view; };
struct Frame   { vk::UniqueFence fence, acquired; vk::UniqueCommandBuffer cmd; bool timed = false; };
struct Target  { Texture depth; vk::UniqueImageView color; vk::UniqueFramebuffer framebuffer;
                 vk::UniqueSemaphore presented; };
struct Swap    { vk::Extent2D extent; vk::UniqueSwapchainKHR chain; std::vector<Target> targets;
                 bool fseAllowed = false; HMONITOR monitor{}; };

using Frustum = std::array<std::array<float, 4>, 6>;

Frustum chunkFrustum(XMFLOAT4X4 const &m)
{
    Frustum f;
    for (int i = 0; i < 6; ++i)
    {
        auto &p = f[i];
        int axis = i / 2;
        float sign = i % 2 ? -1.f : 1.f;
        for (int j = 0; j < 4; ++j)
            p[j] = (i == 4 ? 0 : m.m[j][3]) + sign * m.m[j][axis];
        p[3] += 8 * (std::abs(p[0]) + std::abs(p[1]) + std::abs(p[2]));
    }
    return f;
}
} // namespace

struct Renderer::State
{
    HWND hwnd;
    vk::UniqueInstance instance;
    vk::UniqueSurfaceKHR surface;
    vk::PhysicalDevice physical;
    uint32_t family{};
    vk::UniqueDevice device;
    vk::Queue queue;
    vk::UniqueCommandPool pool;
    vk::UniqueDescriptorSetLayout descriptors;
    vk::UniquePipelineLayout layout;
    vk::Format colorFormat{}, depthFormat{};
    vk::UniqueRenderPass pass;
    vk::UniquePipeline opaquePipeline, cutoutPipeline;
    Texture atlas;
    vk::UniqueSampler sampler;
    vk::UniqueDescriptorPool descriptorPool;
    vk::DescriptorSet set;
    Buffer vertices;
    std::vector<Batch> batches;
    std::vector<uint32_t> visible;
    std::array<Frame, 2> frames;
    size_t frameIndex{};
    std::unique_ptr<Swap> swap;
    vk::UniqueQueryPool timestamps;
    uint64_t tickMask{};
    double msPerTick{}, gpuMs = -1;
    std::string gpuName;

    bool hasFseExt = false, wantFullScreen = true, wantAcquire = true, fseAcquired = false;
    PFN_vkAcquireFullScreenExclusiveModeEXT pfnAcquireFse = nullptr;
    PFN_vkReleaseFullScreenExclusiveModeEXT pfnReleaseFse = nullptr;

    explicit State(HWND window) : hwnd(window)
    {
        std::vector<const char *> exts = {VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_WIN32_SURFACE_EXTENSION_NAME};
        auto instProps = vk::enumerateInstanceExtensionProperties();
        bool hasSurfCaps2 = std::any_of(instProps.begin(), instProps.end(), [](auto e) {
            return strcmp(e.extensionName, VK_KHR_GET_SURFACE_CAPABILITIES_2_EXTENSION_NAME) == 0;
        });
        if (hasSurfCaps2) exts.push_back(VK_KHR_GET_SURFACE_CAPABILITIES_2_EXTENSION_NAME);

        vk::ApplicationInfo appInfo("PRTX Map Viewer", 1, nullptr, 0, VK_API_VERSION_1_0);
        instance = vk::createInstanceUnique(
            vk::InstanceCreateInfo().setPApplicationInfo(&appInfo).setPEnabledExtensionNames(exts));
        surface = instance->createWin32SurfaceKHRUnique(
            vk::Win32SurfaceCreateInfoKHR({}, GetModuleHandleW(nullptr), hwnd));

        for (auto gpu : instance->enumeratePhysicalDevices())
        {
            auto ext = gpu.enumerateDeviceExtensionProperties();
            auto hasExt = [&](const char *name) {
                return std::any_of(ext.begin(), ext.end(), [&](auto e) { return strcmp(e.extensionName, name) == 0; });
            };
            if (!hasExt(VK_KHR_SWAPCHAIN_EXTENSION_NAME)) continue;

            auto families = gpu.getQueueFamilyProperties();
            for (uint32_t i = 0; i < families.size(); ++i)
            {
                if ((families[i].queueFlags & vk::QueueFlagBits::eGraphics) &&
                    gpu.getSurfaceSupportKHR(i, *surface))
                {
                    physical  = gpu;
                    family    = i;
                    hasFseExt = hasSurfCaps2 && hasExt(VK_EXT_FULL_SCREEN_EXCLUSIVE_EXTENSION_NAME);
                    auto props = gpu.getProperties();
                    gpuName    = props.deviceName.data();
                    if (auto bits = families[i].timestampValidBits)
                    {
                        tickMask  = UINT64_MAX >> (64 - bits);
                        msPerTick = props.limits.timestampPeriod * 1e-6;
                    }
                    break;
                }
            }
            if (physical) break;
        }
        if (!physical)
            throw std::runtime_error("No Vulkan graphics device can present to this window.");

        float priority = 1;
        vk::DeviceQueueCreateInfo qi({}, family, 1, &priority);
        std::vector<const char *> devExts = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
        if (hasFseExt) devExts.push_back(VK_EXT_FULL_SCREEN_EXCLUSIVE_EXTENSION_NAME);
        device = physical.createDeviceUnique(vk::DeviceCreateInfo({}, qi, {}, devExts));

        if (hasFseExt)
        {
            pfnAcquireFse = reinterpret_cast<PFN_vkAcquireFullScreenExclusiveModeEXT>(
                vkGetDeviceProcAddr(*device, "vkAcquireFullScreenExclusiveModeEXT"));
            pfnReleaseFse = reinterpret_cast<PFN_vkReleaseFullScreenExclusiveModeEXT>(
                vkGetDeviceProcAddr(*device, "vkReleaseFullScreenExclusiveModeEXT"));
            hasFseExt = pfnAcquireFse && pfnReleaseFse;
        }

        if (tickMask)
            timestamps = device->createQueryPoolUnique(
                {{}, vk::QueryType::eTimestamp, uint32_t(frames.size() * 2)});
        queue = device->getQueue(family, 0);
        pool  = device->createCommandPoolUnique({vk::CommandPoolCreateFlagBits::eResetCommandBuffer, family});
        for (auto &f : frames)
        {
            f.cmd      = command();
            f.acquired = device->createFenceUnique({});
            f.fence    = device->createFenceUnique({vk::FenceCreateFlagBits::eSignaled});
        }

        vk::DescriptorSetLayoutBinding binding(0, vk::DescriptorType::eCombinedImageSampler, 1,
                                               vk::ShaderStageFlagBits::eFragment);
        descriptors = device->createDescriptorSetLayoutUnique(vk::DescriptorSetLayoutCreateInfo({}, binding));
        vk::PushConstantRange pc(vk::ShaderStageFlagBits::eVertex, 0, 64);
        layout = device->createPipelineLayoutUnique(vk::PipelineLayoutCreateInfo({}, *descriptors, pc));
        loadAtlas();
    }

    ~State()
    {
        if (device) { vkDeviceWaitIdle(*device); releaseFse(); }
    }

    // --- Resource & transfer helpers ---

    vk::UniqueCommandBuffer command()
    {
        return std::move(device->allocateCommandBuffersUnique(
            {*pool, vk::CommandBufferLevel::ePrimary, 1})[0]);
    }

    vk::UniqueDeviceMemory allocate(vk::MemoryRequirements r, vk::MemoryPropertyFlags flags)
    {
        auto props = physical.getMemoryProperties();
        for (uint32_t i = 0; i < props.memoryTypeCount; ++i)
            if ((r.memoryTypeBits & (1u << i)) && (props.memoryTypes[i].propertyFlags & flags) == flags)
                return device->allocateMemoryUnique({r.size, i});
        throw std::runtime_error("No suitable Vulkan memory type.");
    }

    Buffer buffer(size_t bytes, vk::BufferUsageFlags usage, vk::MemoryPropertyFlags flags)
    {
        Buffer b;
        b.handle = device->createBufferUnique({{}, bytes, usage});
        b.memory = allocate(device->getBufferMemoryRequirements(*b.handle), flags);
        device->bindBufferMemory(*b.handle, *b.memory, 0);
        return b;
    }

    Buffer stage(const void *data, size_t bytes)
    {
        auto b = buffer(bytes, vk::BufferUsageFlagBits::eTransferSrc,
                        vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent);
        void *dest = device->mapMemory(*b.memory, 0, bytes);
        memcpy(dest, data, bytes);
        device->unmapMemory(*b.memory);
        return b;
    }

    template <class F> void transfer(F record)
    {
        auto c = command();
        c->begin({vk::CommandBufferUsageFlagBits::eOneTimeSubmit});
        record(*c);
        c->end();
        vk::SubmitInfo info;
        info.setCommandBuffers(*c);
        queue.submit(info);
        queue.waitIdle();
    }

    Texture texture(vk::Extent2D size, vk::Format fmt, vk::ImageUsageFlags usage, vk::ImageAspectFlags aspect)
    {
        Texture t;
        t.image  = device->createImageUnique({{}, vk::ImageType::e2D, fmt, {size.width, size.height, 1},
                                              1, 1, vk::SampleCountFlagBits::e1,
                                              vk::ImageTiling::eOptimal, usage});
        t.memory = allocate(device->getImageMemoryRequirements(*t.image),
                            vk::MemoryPropertyFlagBits::eDeviceLocal);
        device->bindImageMemory(*t.image, *t.memory, 0);
        t.view   = device->createImageViewUnique({{}, *t.image, vk::ImageViewType::e2D,
                                                  fmt, {}, {aspect, 0, 1, 0, 1}});
        return t;
    }

    void loadAtlas()
    {
        auto module = GetModuleHandleW(nullptr);
        auto r = FindResourceW(module, MAKEINTRESOURCEW(101), MAKEINTRESOURCEW(10));
        if (!r || SizeofResource(module, r) != 512 * 512 * 4)
            throw std::runtime_error("Embedded atlas is missing.");
        auto staging = stage(LockResource(LoadResource(module, r)), 512 * 512 * 4);
        auto aspect  = vk::ImageAspectFlagBits::eColor;
        atlas = texture({512, 512}, vk::Format::eR8G8B8A8Unorm,
                        vk::ImageUsageFlagBits::eTransferDst | vk::ImageUsageFlagBits::eSampled, aspect);
        transfer([&](vk::CommandBuffer c) {
            vk::ImageMemoryBarrier b({}, vk::AccessFlagBits::eTransferWrite,
                                     vk::ImageLayout::eUndefined, vk::ImageLayout::eTransferDstOptimal,
                                     VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED,
                                     *atlas.image, {aspect, 0, 1, 0, 1});
            c.pipelineBarrier(vk::PipelineStageFlagBits::eTopOfPipe,
                              vk::PipelineStageFlagBits::eTransfer, {}, {}, {}, b);
            c.copyBufferToImage(*staging.handle, *atlas.image, b.newLayout,
                                vk::BufferImageCopy(0, 0, 0, {aspect, 0, 0, 1}, {0, 0, 0}, {512, 512, 1}));
            b.oldLayout = b.newLayout;
            b.newLayout = vk::ImageLayout::eShaderReadOnlyOptimal;
            b.srcAccessMask = b.dstAccessMask;
            b.dstAccessMask = vk::AccessFlagBits::eShaderRead;
            c.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer,
                              vk::PipelineStageFlagBits::eFragmentShader, {}, {}, {}, b);
        });
        sampler = device->createSamplerUnique({});
        vk::DescriptorPoolSize capacity(vk::DescriptorType::eCombinedImageSampler, 1);
        descriptorPool = device->createDescriptorPoolUnique(vk::DescriptorPoolCreateInfo({}, 1, capacity));
        set = device->allocateDescriptorSets(vk::DescriptorSetAllocateInfo(*descriptorPool, *descriptors))[0];
        vk::DescriptorImageInfo imgInfo(*sampler, *atlas.view, vk::ImageLayout::eShaderReadOnlyOptimal);
        device->updateDescriptorSets(
            vk::WriteDescriptorSet(set, 0, 0, vk::DescriptorType::eCombinedImageSampler, imgInfo), {});
    }

    // --- FSE & swapchain lifecycle ---

    void releaseFse()
    {
        if (fseAcquired && swap && pfnReleaseFse) pfnReleaseFse(*device, *swap->chain);
        fseAcquired = false;
    }

    void acquireFse()
    {
        if (!hasFseExt || !wantFullScreen || !wantAcquire || fseAcquired || !swap || !swap->fseAllowed)
            return;
        if (pfnAcquireFse(*device, *swap->chain) == VK_SUCCESS)
            fseAcquired = true;
    }

    void dropSwapchain() { device->waitIdle(); releaseFse(); swap.reset(); }

    void createPipelines(vk::Format colorFmt, vk::Format depthFmt);
    void rebuild(vk::Extent2D size);
    void draw(Camera const &, bool);
};

Renderer::Renderer(HWND h) : s(std::make_unique<State>(h)) {}
Renderer::~Renderer() = default;

void Renderer::setFullScreen(bool fullscreen, bool acquire)
{
    bool modeChanged  = (s->wantFullScreen != fullscreen);
    s->wantFullScreen = fullscreen;
    s->wantAcquire    = fullscreen && acquire;
    if (modeChanged) { s->dropSwapchain(); return; }
    if (s->wantAcquire) s->acquireFse();
    else                s->releaseFse();
}

bool Renderer::isFullScreenExclusive() const { return s->fseAcquired; }

void Renderer::upload(Map &map)
{
    Buffer fresh;
    if (!map.quads.empty())
    {
        auto bytes   = map.quads.size() * sizeof(Quad);
        auto staging = s->stage(map.quads.data(), bytes);
        fresh = s->buffer(bytes,
                    vk::BufferUsageFlagBits::eVertexBuffer | vk::BufferUsageFlagBits::eTransferDst,
                    vk::MemoryPropertyFlagBits::eDeviceLocal);
        s->transfer([&](vk::CommandBuffer c) {
            c.copyBuffer(*staging.handle, *fresh.handle, vk::BufferCopy(0, 0, bytes));
            vk::MemoryBarrier barrier(vk::AccessFlagBits::eTransferWrite,
                                     vk::AccessFlagBits::eVertexAttributeRead);
            c.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer,
                              vk::PipelineStageFlagBits::eVertexInput, {}, barrier, {}, {});
        });
    }
    else
        s->device->waitIdle();
    std::swap(s->vertices, fresh);
    s->batches = std::move(map.batches);
}

void Renderer::draw(Camera const &camera, bool measure) { s->draw(camera, measure); }
double Renderer::gpuMilliseconds() const { return s->gpuMs; }
const std::string &Renderer::adapterName() const { return s->gpuName; }

void Renderer::State::createPipelines(vk::Format colorFmt, vk::Format depthFmt)
{
    colorFormat = colorFmt;
    depthFormat = depthFmt;

    vk::AttachmentDescription attachments[2];
    for (auto &a : attachments)
    {
        a.samples        = vk::SampleCountFlagBits::e1;
        a.loadOp         = vk::AttachmentLoadOp::eClear;
        a.stencilLoadOp  = vk::AttachmentLoadOp::eDontCare;
        a.stencilStoreOp = vk::AttachmentStoreOp::eDontCare;
    }
    attachments[0].format      = colorFmt;
    attachments[0].storeOp     = vk::AttachmentStoreOp::eStore;
    attachments[0].finalLayout = vk::ImageLayout::ePresentSrcKHR;
    attachments[1].format      = depthFmt;
    attachments[1].storeOp     = vk::AttachmentStoreOp::eDontCare;
    attachments[1].finalLayout = vk::ImageLayout::eDepthStencilAttachmentOptimal;

    vk::AttachmentReference colorRef(0, vk::ImageLayout::eColorAttachmentOptimal);
    vk::AttachmentReference depthRef(1, vk::ImageLayout::eDepthStencilAttachmentOptimal);
    vk::SubpassDescription  sub({}, vk::PipelineBindPoint::eGraphics, {}, colorRef, {}, &depthRef);
    auto stagesMask = vk::PipelineStageFlagBits::eColorAttachmentOutput |
                      vk::PipelineStageFlagBits::eEarlyFragmentTests |
                      vk::PipelineStageFlagBits::eLateFragmentTests;
    vk::SubpassDependency dep(VK_SUBPASS_EXTERNAL, 0, stagesMask, stagesMask, {},
                              vk::AccessFlagBits::eColorAttachmentWrite |
                              vk::AccessFlagBits::eDepthStencilAttachmentWrite);
    pass = device->createRenderPassUnique(vk::RenderPassCreateInfo({}, attachments, sub, dep));

    auto vs = device->createShaderModuleUnique({{}, sizeof(vertexCode), vertexCode});
    auto fs = device->createShaderModuleUnique({{}, sizeof(fragmentCode), fragmentCode});

    vk::VertexInputBindingDescription binding(0, sizeof(Quad), vk::VertexInputRate::eInstance);
    vk::VertexInputAttributeDescription attrs[] = {
        {0, 0, vk::Format::eR32G32B32Sfloat,    offsetof(Quad, origin)},
        {1, 0, vk::Format::eR32G32B32A32Sfloat, offsetof(Quad, width)},
        {2, 0, vk::Format::eR32Uint,             offsetof(Quad, face)},
        {3, 0, vk::Format::eR32Sint,             offsetof(Quad, material)}};
    vk::PipelineVertexInputStateCreateInfo   input({}, binding, attrs);
    vk::PipelineInputAssemblyStateCreateInfo assembly({}, vk::PrimitiveTopology::eTriangleStrip);
    vk::PipelineViewportStateCreateInfo      viewState({}, 1, nullptr, 1, nullptr);
    vk::DynamicState dynStates[] = {vk::DynamicState::eViewport, vk::DynamicState::eScissor};
    vk::PipelineDynamicStateCreateInfo       dynamic({}, dynStates);
    vk::PipelineRasterizationStateCreateInfo raster({}, false, false, vk::PolygonMode::eFill,
                                                    vk::CullModeFlagBits::eBack,
                                                    vk::FrontFace::eCounterClockwise,
                                                    false, 0, 0, 0, 1);
    vk::PipelineMultisampleStateCreateInfo   ms({}, vk::SampleCountFlagBits::e1);
    vk::PipelineDepthStencilStateCreateInfo  depth({}, true, true, vk::CompareOp::eLess);
    vk::PipelineColorBlendAttachmentState    colorBlend;
    colorBlend.colorWriteMask = vk::ColorComponentFlags(15);
    vk::PipelineColorBlendStateCreateInfo    blend({}, false, vk::LogicOp::eCopy, colorBlend);

    auto makePipeline = [&](vk::Bool32 cutout) {
        vk::SpecializationMapEntry entry(0, 0, sizeof(vk::Bool32));
        vk::SpecializationInfo spec(1, &entry, sizeof(vk::Bool32), &cutout);
        vk::PipelineShaderStageCreateInfo stages[] = {
            {{}, vk::ShaderStageFlagBits::eVertex,   *vs, "main"},
            {{}, vk::ShaderStageFlagBits::eFragment, *fs, "main", &spec}};
        vk::GraphicsPipelineCreateInfo info({}, stages, &input, &assembly, nullptr, &viewState,
                                            &raster, &ms, &depth, &blend, &dynamic, *layout, *pass);
        return device->createGraphicsPipelineUnique({}, info).value;
    };
    opaquePipeline = makePipeline(VK_FALSE);
    cutoutPipeline = makePipeline(VK_TRUE);
}

void Renderer::State::rebuild(vk::Extent2D size)
{
    device->waitIdle();
    releaseFse();

    HMONITOR monitor = MonitorFromWindow(hwnd, MONITOR_DEFAULTTOPRIMARY);
    bool fse = hasFseExt && wantFullScreen && monitor != nullptr;
    vk::SurfaceFullScreenExclusiveWin32InfoEXT win32Info(monitor);

    vk::SurfaceCapabilitiesKHR caps;
    if (fse)
    {
        vk::SurfaceCapabilitiesFullScreenExclusiveEXT fseCaps;
        vk::SurfaceCapabilities2KHR caps2;
        caps2.pNext = &fseCaps;
        vk::PhysicalDeviceSurfaceInfo2KHR surfInfo(*surface, &win32Info);
        if (physical.getSurfaceCapabilities2KHR(&surfInfo, &caps2) == vk::Result::eSuccess)
        {
            caps = caps2.surfaceCapabilities;
            fse  = fseCaps.fullScreenExclusiveSupported == VK_TRUE;
        }
        else
            fse = false;
    }
    if (!fse) caps = physical.getSurfaceCapabilitiesKHR(*surface);

    auto formats = physical.getSurfaceFormatsKHR(*surface);
    auto choice  = std::find_if(formats.begin(), formats.end(), [](auto f) {
        return f.format == vk::Format::eB8G8R8A8Unorm || f.format == vk::Format::eR8G8B8A8Unorm;
    });
    if (choice == formats.end())
        throw std::runtime_error("The Vulkan surface needs an RGBA8 or BGRA8 UNORM format.");

    auto depthFmt = vk::Format::eD32Sfloat;
    if (!(physical.getFormatProperties(depthFmt).optimalTilingFeatures &
          vk::FormatFeatureFlagBits::eDepthStencilAttachment))
        depthFmt = vk::Format::eD16Unorm;

    if (!pass || colorFormat != choice->format || depthFormat != depthFmt)
        createPipelines(choice->format, depthFmt);

    auto modes = physical.getSurfacePresentModesKHR(*surface);
    bool mailbox = std::find(modes.begin(), modes.end(), vk::PresentModeKHR::eMailbox) != modes.end();
    uint32_t count = std::max(caps.minImageCount, mailbox ? 3u : 2u);
    if (caps.maxImageCount) count = std::min(count, caps.maxImageCount);

    auto next = std::make_unique<Swap>();
    next->extent     = size;
    next->fseAllowed = fse;
    next->monitor    = monitor;

    vk::SurfaceFullScreenExclusiveInfoEXT fseInfo(
        vk::FullScreenExclusiveEXT::eApplicationControlled, &win32Info);
    vk::SwapchainCreateInfoKHR ci({}, *surface, count, choice->format, choice->colorSpace,
                                  size, 1, vk::ImageUsageFlagBits::eColorAttachment);
    ci.preTransform   = caps.currentTransform;
    ci.compositeAlpha = vk::CompositeAlphaFlagBitsKHR::eOpaque;
    ci.presentMode    = mailbox ? vk::PresentModeKHR::eMailbox : vk::PresentModeKHR::eFifo;
    ci.clipped        = true;
    if (fse)  ci.pNext = &fseInfo;
    if (swap) ci.oldSwapchain = *swap->chain;
    next->chain = device->createSwapchainKHRUnique(ci);

    for (auto image : device->getSwapchainImagesKHR(*next->chain))
    {
        Target t;
        t.depth = texture(size, depthFmt, vk::ImageUsageFlagBits::eDepthStencilAttachment,
                          vk::ImageAspectFlagBits::eDepth);
        t.color = device->createImageViewUnique({{}, image, vk::ImageViewType::e2D, choice->format,
                                                 {}, {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1}});
        vk::ImageView views[] = {*t.color, *t.depth.view};
        t.framebuffer = device->createFramebufferUnique(
            vk::FramebufferCreateInfo({}, *pass, views, size.width, size.height, 1));
        t.presented = device->createSemaphoreUnique({});
        next->targets.push_back(std::move(t));
    }
    swap = std::move(next);
    acquireFse();
}

void Renderer::State::draw(Camera const &cam, bool measure)
{
    RECT rect;
    GetClientRect(hwnd, &rect);
    if (!rect.right || !rect.bottom) return;
    vk::Extent2D size(uint32_t(rect.right), uint32_t(rect.bottom));
    HMONITOR monitor = MonitorFromWindow(hwnd, MONITOR_DEFAULTTOPRIMARY);
    if (!swap || swap->extent != size || (wantFullScreen && swap->monitor != monitor))
        rebuild(size);
    if (wantFullScreen && wantAcquire && !fseAcquired)
        acquireFse();

    auto &frame = frames[frameIndex];
    if (device->waitForFences(*frame.fence, true, UINT64_MAX) != vk::Result::eSuccess)
        throw std::runtime_error("Vulkan frame wait failed.");

    const uint32_t query = uint32_t(frameIndex * 2);
    if (frame.timed)
    {
        uint64_t ticks[2];
        if (device->getQueryPoolResults(*timestamps, query, 2, sizeof(ticks), ticks,
                                        sizeof(uint64_t), vk::QueryResultFlagBits::e64) == vk::Result::eSuccess)
            gpuMs = double((ticks[1] - ticks[0]) & tickMask) * msPerTick;
    }

    uint32_t image;
    auto acquired = device->acquireNextImageKHR(*swap->chain, UINT64_MAX, {}, *frame.acquired, &image);
    if (acquired == vk::Result::eErrorOutOfDateKHR ||
        acquired == vk::Result::eErrorFullScreenExclusiveModeLostEXT)
    {
        if (acquired == vk::Result::eErrorFullScreenExclusiveModeLostEXT) fseAcquired = false;
        dropSwapchain();
        return;
    }
    if (acquired != vk::Result::eSuccess && acquired != vk::Result::eSuboptimalKHR)
        throw std::runtime_error("Could not acquire a swapchain image.");
    if (device->waitForFences(*frame.acquired, true, UINT64_MAX) != vk::Result::eSuccess)
        throw std::runtime_error("Swapchain image wait failed.");
    device->resetFences(*frame.acquired);

    auto &target = swap->targets[image];
    auto  c      = *frame.cmd;
    c.reset();
    c.begin({vk::CommandBufferUsageFlagBits::eOneTimeSubmit});
    frame.timed = measure && bool(timestamps);
    if (frame.timed)
    {
        c.resetQueryPool(*timestamps, query, 2);
        c.writeTimestamp(vk::PipelineStageFlagBits::eTopOfPipe, *timestamps, query);
    }

    vk::ClearValue clears[] = {vk::ClearColorValue(std::array<float, 4>{.20f, .26f, .32f, 1}),
                               vk::ClearDepthStencilValue(1, 0)};
    c.beginRenderPass(vk::RenderPassBeginInfo(*pass, *target.framebuffer, {{0,0},size}, clears),
                      vk::SubpassContents::eInline);

    if (vertices.handle)
    {
        float cp = std::cos(cam.pitch);
        auto view = XMMatrixLookToRH(
            XMVectorSet(cam.pos.x, cam.pos.y, cam.pos.z, 1),
            XMVectorSet(-std::sin(cam.yaw)*cp, std::sin(cam.pitch), -std::cos(cam.yaw)*cp, 0),
            XMVectorSet(0, 1, 0, 0));
        auto proj = XMMatrixPerspectiveFovRH(XM_PIDIV2, float(size.width)/size.height, .02f, 500) *
                    XMMatrixScaling(1, -1, 1);
        XMFLOAT4X4 vp;
        XMStoreFloat4x4(&vp, view * proj);

        c.setViewport(0, vk::Viewport(0, 0, float(size.width), float(size.height), 0, 1));
        c.setScissor(0, vk::Rect2D({0, 0}, size));
        c.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, *layout, 0, set, {});
        c.pushConstants(*layout, vk::ShaderStageFlagBits::eVertex, 0, 64, &vp);
        vk::DeviceSize offset = 0;
        c.bindVertexBuffers(0, *vertices.handle, offset);

        const auto planes = chunkFrustum(vp);
        visible.clear();
        bool hasOpaque = false, hasCutout = false;
        for (uint32_t i = 0; i < uint32_t(batches.size()); ++i)
        {
            const auto &b = batches[i];
            if (std::all_of(planes.begin(), planes.end(), [&](const auto &p) {
                    return b.center.x*p[0] + b.center.y*p[1] + b.center.z*p[2] + p[3] >= 0;
                }))
            {
                visible.push_back(i);
                hasOpaque |= b.opaqueCount > 0;
                hasCutout |= b.opaqueCount < b.count;
            }
        }

        auto drawPass = [&](vk::Pipeline pipe, bool cutout) {
            c.bindPipeline(vk::PipelineBindPoint::eGraphics, pipe);
            uint32_t first = 0, count = 0;
            for (uint32_t idx : visible)
            {
                const auto &b = batches[idx];
                uint32_t n = cutout ? (b.count - b.opaqueCount) : b.opaqueCount;
                if (!n) continue;
                uint32_t f = cutout ? (b.first + b.opaqueCount) : b.first;
                if (count && first + count == f)
                    count += n;
                else
                {
                    if (count) c.draw(4, count, 0, first);
                    first = f;
                    count = n;
                }
            }
            if (count) c.draw(4, count, 0, first);
        };
        if (hasOpaque) drawPass(*opaquePipeline, false);
        if (hasCutout) drawPass(*cutoutPipeline, true);
    }

    c.endRenderPass();
    if (frame.timed)
        c.writeTimestamp(vk::PipelineStageFlagBits::eBottomOfPipe, *timestamps, query + 1);
    c.end();

    device->resetFences(*frame.fence);
    vk::SubmitInfo submit;
    submit.setCommandBuffers(c).setSignalSemaphores(*target.presented);
    queue.submit(submit, *frame.fence);

    vk::PresentInfoKHR present(*target.presented, *swap->chain, image);
    auto result = queue.presentKHR(&present);
    if (result == vk::Result::eErrorOutOfDateKHR || result == vk::Result::eSuboptimalKHR ||
        result == vk::Result::eErrorFullScreenExclusiveModeLostEXT ||
        acquired == vk::Result::eSuboptimalKHR)
    {
        if (result == vk::Result::eErrorFullScreenExclusiveModeLostEXT) fseAcquired = false;
        dropSwapchain();
    }
    else if (result != vk::Result::eSuccess)
        throw std::runtime_error("Vulkan presentation failed.");

    frameIndex = (frameIndex + 1) % frames.size();
}
