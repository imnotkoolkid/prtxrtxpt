#pragma once
#include <vulkan/vulkan.hpp>
#include <cstring>
#include <stdexcept>

struct Buffer
{
    vk::UniqueDeviceMemory memory;
    vk::UniqueBuffer handle;
};
struct Texture
{
    vk::UniqueDeviceMemory memory;
    vk::UniqueImage image;
    vk::UniqueImageView view;
};

struct Resources
{
    vk::PhysicalDevice physical;
    vk::Device device;
    vk::UniqueDeviceMemory allocate(vk::MemoryRequirements r, vk::MemoryPropertyFlags flags)
    {
        auto props = physical.getMemoryProperties();
        for (uint32_t i = 0; i < props.memoryTypeCount; ++i)
            if ((r.memoryTypeBits & (1u << i)) && (props.memoryTypes[i].propertyFlags & flags) == flags)
                return device.allocateMemoryUnique({r.size, i});
        throw std::runtime_error("No suitable Vulkan memory type.");
    }

    Buffer buffer(size_t bytes, vk::BufferUsageFlags usage, vk::MemoryPropertyFlags flags)
    {
        Buffer b;
        b.handle = device.createBufferUnique({{}, bytes, usage});
        b.memory = allocate(device.getBufferMemoryRequirements(*b.handle), flags);
        device.bindBufferMemory(*b.handle, *b.memory, 0);
        return b;
    }

    Buffer stage(const void *data, size_t bytes)
    {
        auto b = buffer(bytes, vk::BufferUsageFlagBits::eTransferSrc,
                        vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent);
        void *dest = device.mapMemory(*b.memory, 0, bytes);
        memcpy(dest, data, bytes);
        device.unmapMemory(*b.memory);
        return b;
    }

    Texture texture(vk::Extent2D size, vk::Format fmt, vk::ImageUsageFlags usage, vk::ImageAspectFlags aspect)
    {
        Texture t;
        t.image = device.createImageUnique({{},
                                            vk::ImageType::e2D,
                                            fmt,
                                            {size.width, size.height, 1},
                                            1,
                                            1,
                                            vk::SampleCountFlagBits::e1,
                                            vk::ImageTiling::eOptimal,
                                            usage});
        t.memory =
            allocate(device.getImageMemoryRequirements(*t.image), vk::MemoryPropertyFlagBits::eDeviceLocal);
        device.bindImageMemory(*t.image, *t.memory, 0);
        t.view =
            device.createImageViewUnique({{}, *t.image, vk::ImageViewType::e2D, fmt, {}, {aspect, 0, 1, 0, 1}});
        return t;
    }
};

class MappedBuffer
{
  public:
    Buffer buffer;
    void *data{};
    void reserve(Resources &gpu, size_t bytes, vk::BufferUsageFlags usage)
    {
        if (bytes <= capacity)
            return;
        auto next = gpu.buffer(
            bytes, usage, vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent);
        auto mapping = gpu.device.mapMemory(*next.memory, 0, bytes);
        if (data)
            device.unmapMemory(*buffer.memory);
        std::swap(buffer, next);
        data = mapping;
        device = gpu.device;
        capacity = bytes;
    }
    ~MappedBuffer()
    {
        if (data)
            device.unmapMemory(*buffer.memory);
    }
    MappedBuffer() = default;
    MappedBuffer(const MappedBuffer &) = delete;
    MappedBuffer &operator=(const MappedBuffer &) = delete;

  private:
    vk::Device device;
    size_t capacity{};
};
