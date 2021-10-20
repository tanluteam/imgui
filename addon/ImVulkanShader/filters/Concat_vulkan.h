#pragma once
#include <imgui.h>
#include "imvk_gpu.h"
#include "imvk_pipeline.h"
#include <imgui_mat.h>

#define CONCAT_HORIZONTAL   0
#define CONCAT_VERTICAL     1

namespace ImGui 
{
class IMGUI_API Concat_vulkan
{
public:
    Concat_vulkan(int gpu = -1);
    ~Concat_vulkan();
    // direction = 0 means horizontal
    // direction = 1 means vertical
    // input CPU Buffer and output to RGBA CPU buffer
    virtual void concat(const ImMat& src0, const ImMat& src1, ImMat& dst, int direction) const;
    // input CPU Buffer and output to RGBA GPU buffer
    virtual void concat(const ImMat& src0, const ImMat& src1, VkMat& dst, int direction) const;
    // input GPU Buffer and output to RGBA CPU buffer
    virtual void concat(const VkMat& src0, const VkMat& src1, ImMat& dst, int direction) const;
    // input GPU Buffer and output to RGBA GPU buffer
    virtual void concat(const VkMat& src0, const VkMat& src1, VkMat& dst, int direction) const;

public:
    const VulkanDevice* vkdev {nullptr};
    Pipeline * pipe           {nullptr};
    VkCompute * cmd           {nullptr};
    Option opt;

private:
    std::vector<uint32_t> spirv_data;
    void upload_param(const VkMat& src0, const VkMat& src1, VkMat& dst, int direction) const;
};
} // namespace ImGui 