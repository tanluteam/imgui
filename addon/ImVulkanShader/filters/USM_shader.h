#pragma once
#include <vk_mat_shader.h>

#define USM_SHADER_PARAM \
" \n\
layout (push_constant) uniform parameter \n\
{ \n\
    int w; \n\
    int h; \n\
    int cstep; \n\
\n\
    int format; \n\
\n\
    float amount; \n\
    float threshold; \n\
} p; \
"

#define SHADER_LOAD_BLUR_RGB \
" \n\
sfpvec3 load_blur_rgb(int x, int y, int w, int cstep, int format) \n\
{ \n\
    sfpvec3 rgb_in = {0.f, 0.f, 0.f}; \n\
    ivec4 i_offset = (y * w + x) * cstep + (format == CF_ABGR ? ivec4(0, 1, 2, 3) : ivec4(0, 3, 2, 1)); \n\
    rgb_in.r = sfp(uint(blur_int8_data[i_offset.r])) / sfp(255.f); \n\
    rgb_in.g = sfp(uint(blur_int8_data[i_offset.g])) / sfp(255.f); \n\
    rgb_in.b = sfp(uint(blur_int8_data[i_offset.b])) / sfp(255.f); \n\
    return rgb_in; \n\
} \
"

#define SHADER_USM_MAIN \
" \n\
void main() \n\
{ \n\
    int gx = int(gl_GlobalInvocationID.x); \n\
    int gy = int(gl_GlobalInvocationID.y); \n\
    int gz = int(gl_GlobalInvocationID.z); \n\
    if (gx >= p.w || gy >= p.h || gz >= 3) \n\
        return; \n\
    sfpvec3 src_rgb = load_src_rgb(gx, gy, p.w, p.cstep, p.format); \n\
    sfpvec3 blur_rgb = load_blur_rgb(gx, gy, p.w, p.cstep, p.format); \n\
    sfpvec3 diff = abs(src_rgb - blur_rgb); \n\
    sfpvec3 result = src_rgb * (sfp(1.f) + p.amount) - blur_rgb * p.amount; \n\
    if (diff.r < p.threshold && diff.g < p.threshold && diff.b < p.threshold) \n\
        result = src_rgb; \n\
    store_dst_rgb(result, gx, gy, p.w, p.cstep, p.format); \n\
} \
"

static const char USMFilter_data[] = 
SHADER_HEADER
R"(
layout (binding = 0) readonly buffer src_int8 { uint8_t src_int8_data[]; };
layout (binding = 1) readonly buffer blur_int8 { uint8_t blur_int8_data[]; };
layout (binding = 2) writeonly buffer dst_int8 { uint8_t dst_int8_data[]; };
)"
USM_SHADER_PARAM
SHADER_LOAD_SRC_RGB
SHADER_LOAD_BLUR_RGB
SHADER_STORE_DST_RGB
SHADER_USM_MAIN
;
