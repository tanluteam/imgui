#pragma once
#include <imgui.h>
#include "gpu.h"
#include "pipeline.h"
#include <imgui_mat.h>

namespace ImGui 
{
class IMGUI_API Exposure_vulkan
{
public:
    Exposure_vulkan(int gpu = -1);
    ~Exposure_vulkan();

    // input CPU Buffer and output to RGBA CPU buffer
    virtual void filter(const ImMat& src, ImMat& dst, float exposure) const;
    // input CPU Buffer and output to RGBA GPU buffer
    virtual void filter(const ImMat& src, VkMat& dst, float exposure) const;
    // input GPU Buffer and output to RGBA CPU buffer
    virtual void filter(const VkMat& src, ImMat& dst, float exposure) const;
    // input GPU Buffer and output to RGBA GPU buffer
    virtual void filter(const VkMat& src, VkMat& dst, float exposure) const;

public:
    const VulkanDevice* vkdev {nullptr};
    Pipeline * pipe           {nullptr};
    VkCompute * cmd           {nullptr};
    Option opt;

private:
    void upload_param(const VkMat& src, VkMat& dst, float exposure) const;
};
} // namespace ImGui 