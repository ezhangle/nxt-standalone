// Copyright 2017 The NXT Authors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "backend/vulkan/TextureVk.h"

#include "backend/vulkan/FencedDeleter.h"
#include "backend/vulkan/VulkanBackend.h"

namespace backend { namespace vulkan {

    namespace {
        // Converts an NXT texture dimension to a Vulkan image type.
        // Note that in Vulkan dimensionality is only 1D, 2D, 3D. Arrays and cube maps are expressed
        // via the array size and a "cubemap compatible" flag.
        VkImageType VulkanImageType(nxt::TextureDimension dimension) {
            switch (dimension) {
                case nxt::TextureDimension::e2D:
                    return VK_IMAGE_TYPE_2D;
                default:
                    UNREACHABLE();
            }
        }

        // Converts NXT texture format to Vulkan formats.
        VkFormat VulkanImageFormat(nxt::TextureFormat format) {
            switch (format) {
                case nxt::TextureFormat::R8G8B8A8Unorm:
                    return VK_FORMAT_R8G8B8A8_UNORM;
                case nxt::TextureFormat::R8G8B8A8Uint:
                    return VK_FORMAT_R8G8B8A8_UINT;
                case nxt::TextureFormat::B8G8R8A8Unorm:
                    return VK_FORMAT_B8G8R8A8_UNORM;
                case nxt::TextureFormat::D32FloatS8Uint:
                    return VK_FORMAT_D32_SFLOAT_S8_UINT;
                default:
                    UNREACHABLE();
            }
        }

        // Converts the NXT usage flags to Vulkan usage flags. Also needs the format to choose
        // between color and depth attachment usages.
        VkImageUsageFlags VulkanImageUsage(nxt::TextureUsageBit usage, nxt::TextureFormat format) {
            VkImageUsageFlags flags = 0;

            if (usage & nxt::TextureUsageBit::TransferSrc) {
                flags |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
            }
            if (usage & nxt::TextureUsageBit::TransferDst) {
                flags |= VK_IMAGE_USAGE_TRANSFER_DST_BIT;
            }
            if (usage & nxt::TextureUsageBit::Sampled) {
                flags |= VK_IMAGE_USAGE_SAMPLED_BIT;
            }
            if (usage & nxt::TextureUsageBit::Storage) {
                flags |= VK_IMAGE_USAGE_STORAGE_BIT;
            }
            if (usage & nxt::TextureUsageBit::OutputAttachment) {
                if (TextureFormatHasDepthOrStencil(format)) {
                    flags |= VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
                } else {
                    flags |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
                }
            }

            return flags;
        }

        // Computes which vulkan access type could be required for the given NXT usage.
        VkAccessFlags VulkanAccessFlags(nxt::TextureUsageBit usage, nxt::TextureFormat format) {
            VkAccessFlags flags = 0;

            if (usage & nxt::TextureUsageBit::TransferSrc) {
                flags |= VK_ACCESS_TRANSFER_READ_BIT;
            }
            if (usage & nxt::TextureUsageBit::TransferDst) {
                flags |= VK_ACCESS_TRANSFER_WRITE_BIT;
            }
            if (usage & nxt::TextureUsageBit::Sampled) {
                flags |= VK_ACCESS_SHADER_READ_BIT;
            }
            if (usage & nxt::TextureUsageBit::Storage) {
                flags |= VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
            }
            if (usage & nxt::TextureUsageBit::OutputAttachment) {
                if (TextureFormatHasDepthOrStencil(format)) {
                    flags |= VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                             VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
                } else {
                    flags |=
                        VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
                }
            }

            // TODO(cwallez@chromium.org): What about present? Does it require VK_MEMORY_READ_BIT?

            return flags;
        }

        // Chooses which Vulkan image layout should be used for the given NXT usage
        VkImageLayout VulkanImageLayout(nxt::TextureUsageBit usage, nxt::TextureFormat format) {
            if (usage == nxt::TextureUsageBit::None) {
                return VK_IMAGE_LAYOUT_UNDEFINED;
            }

            if (!nxt::HasZeroOrOneBits(usage)) {
                return VK_IMAGE_LAYOUT_GENERAL;
            }

            // Usage has a single bit so we can switch on its value directly.
            switch (usage) {
                case nxt::TextureUsageBit::TransferDst:
                    return VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
                case nxt::TextureUsageBit::Sampled:
                    return VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                // Vulkan texture copy functions require the image to be in _one_  known layout.
                // Depending on whether parts of the texture have been transitioned to only
                // TransferSrc or a combination with something else, the texture could be in a
                // combination of GENERAL and TRANSFER_SRC_OPTIMAL. This would be a problem, so we
                // make TransferSrc use GENERAL.
                case nxt::TextureUsageBit::TransferSrc:
                // Writable storage textures must use general. If we could know the texture is read
                // only we could use SHADER_READ_ONLY_OPTIMAL
                case nxt::TextureUsageBit::Storage:
                case nxt::TextureUsageBit::Present:
                    return VK_IMAGE_LAYOUT_GENERAL;
                case nxt::TextureUsageBit::OutputAttachment:
                    if (TextureFormatHasDepthOrStencil(format)) {
                        return VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
                    } else {
                        return VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
                    }
                default:
                    UNREACHABLE();
            }
        }

        // Computes which Vulkan pipeline stage can access a texture in the given NXT usage
        VkPipelineStageFlags VulkanPipelineStage(nxt::TextureUsageBit usage,
                                                 nxt::TextureFormat format) {
            VkPipelineStageFlags flags = 0;

            if (usage == nxt::TextureUsageBit::None) {
                // This only happens when a texture is initially created (and for srcAccessMask) in
                // which case there is no need to wait on anything to stop accessing this texture.
                return VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
            }
            if (usage & (nxt::TextureUsageBit::TransferSrc | nxt::TextureUsageBit::TransferDst)) {
                flags |= VK_PIPELINE_STAGE_TRANSFER_BIT;
            }
            if (usage & (nxt::TextureUsageBit::Sampled | nxt::TextureUsageBit::Storage)) {
                flags |= VK_PIPELINE_STAGE_VERTEX_SHADER_BIT |
                         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
            }
            if (usage & nxt::TextureUsageBit::OutputAttachment) {
                if (TextureFormatHasDepthOrStencil(format)) {
                    flags |= VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
                             VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
                    // TODO(cwallez@chromium.org): This is missing the stage where the depth and
                    // stencil values are written, but it isn't clear which one it is.
                } else {
                    flags |= VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
                }
            }

            // TODO(cwallez@chromium.org) What about present?

            return flags;
        }

        // Computes which Vulkan texture aspects are relevant for the given NXT format
        VkImageAspectFlags VulkanAspectMask(nxt::TextureFormat format) {
            bool isDepth = TextureFormatHasDepth(format);
            bool isStencil = TextureFormatHasStencil(format);

            VkImageAspectFlags flags = 0;
            if (isDepth) {
                flags |= VK_IMAGE_ASPECT_DEPTH_BIT;
            }
            if (isStencil) {
                flags |= VK_IMAGE_ASPECT_STENCIL_BIT;
            }

            if (flags != 0) {
                return flags;
            }
            return VK_IMAGE_ASPECT_COLOR_BIT;
        }

    }  // namespace

    Texture::Texture(TextureBuilder* builder) : TextureBase(builder) {
        Device* device = ToBackend(GetDevice());

        // Create the Vulkan image "container". We don't need to check that the format supports the
        // combination of sample, usage etc. because validation should have been done in the NXT
        // frontend already based on the minimum supported formats in the Vulkan spec
        VkImageCreateInfo createInfo;
        createInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        createInfo.pNext = nullptr;
        createInfo.flags = 0;
        createInfo.imageType = VulkanImageType(GetDimension());
        createInfo.format = VulkanImageFormat(GetFormat());
        createInfo.extent = VkExtent3D{GetWidth(), GetHeight(), GetDepth()};
        createInfo.mipLevels = GetNumMipLevels();
        createInfo.arrayLayers = 1;
        createInfo.samples = VK_SAMPLE_COUNT_1_BIT;
        createInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
        createInfo.usage = VulkanImageUsage(GetAllowedUsage(), GetFormat());
        createInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        createInfo.queueFamilyIndexCount = 0;
        createInfo.pQueueFamilyIndices = nullptr;
        createInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

        if (device->fn.CreateImage(device->GetVkDevice(), &createInfo, nullptr, &mHandle) !=
            VK_SUCCESS) {
            ASSERT(false);
        }

        // Create the image memory and associate it with the container
        VkMemoryRequirements requirements;
        device->fn.GetImageMemoryRequirements(device->GetVkDevice(), mHandle, &requirements);

        if (!device->GetMemoryAllocator()->Allocate(requirements, false, &mMemoryAllocation)) {
            ASSERT(false);
        }

        if (device->fn.BindImageMemory(device->GetVkDevice(), mHandle,
                                       mMemoryAllocation.GetMemory(),
                                       mMemoryAllocation.GetMemoryOffset()) != VK_SUCCESS) {
            ASSERT(false);
        }
    }

    Texture::~Texture() {
        Device* device = ToBackend(GetDevice());

        // We need to free both the memory allocation and the container. Memory should be freed
        // after the VkImage is destroyed and this is taken care of by the FencedDeleter.
        device->GetMemoryAllocator()->Free(&mMemoryAllocation);

        if (mHandle != VK_NULL_HANDLE) {
            device->GetFencedDeleter()->DeleteWhenUnused(mHandle);
            mHandle = VK_NULL_HANDLE;
        }
    }

    VkImage Texture::GetHandle() const {
        return mHandle;
    }

    VkImageAspectFlags Texture::GetVkAspectMask() const {
        return VulkanAspectMask(GetFormat());
    }

    // Helper function to add a texture barrier to a command buffer. This is inefficient because we
    // should be coalescing barriers as much as possible.
    void Texture::RecordBarrier(VkCommandBuffer commands,
                                nxt::TextureUsageBit currentUsage,
                                nxt::TextureUsageBit targetUsage) const {
        nxt::TextureFormat format = GetFormat();
        VkPipelineStageFlags srcStages = VulkanPipelineStage(currentUsage, format);
        VkPipelineStageFlags dstStages = VulkanPipelineStage(targetUsage, format);

        VkImageMemoryBarrier barrier;
        barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        barrier.pNext = nullptr;
        barrier.srcAccessMask = VulkanAccessFlags(currentUsage, format);
        barrier.dstAccessMask = VulkanAccessFlags(targetUsage, format);
        barrier.oldLayout = VulkanImageLayout(currentUsage, format);
        barrier.newLayout = VulkanImageLayout(targetUsage, format);
        barrier.srcQueueFamilyIndex = 0;
        barrier.dstQueueFamilyIndex = 0;
        barrier.image = mHandle;
        // This transitions the whole resource but assumes it is a 2D texture
        ASSERT(GetDimension() == nxt::TextureDimension::e2D);
        barrier.subresourceRange.aspectMask = VulkanAspectMask(format);
        barrier.subresourceRange.baseMipLevel = 0;
        barrier.subresourceRange.levelCount = GetNumMipLevels();
        barrier.subresourceRange.baseArrayLayer = 0;
        barrier.subresourceRange.layerCount = 1;

        ToBackend(GetDevice())
            ->fn.CmdPipelineBarrier(commands, srcStages, dstStages, 0, 0, nullptr, 0, nullptr, 1,
                                    &barrier);
    }

    void Texture::TransitionUsageImpl(nxt::TextureUsageBit currentUsage,
                                      nxt::TextureUsageBit targetUsage) {
        VkCommandBuffer commands = ToBackend(GetDevice())->GetPendingCommandBuffer();
        RecordBarrier(commands, currentUsage, targetUsage);
    }

}}  // namespace backend::vulkan
