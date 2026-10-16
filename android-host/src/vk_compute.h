#pragma once
#include <stddef.h>
#include <stdint.h>
#include "nvn/nvn_tex.h"
enum { DM_BIND_SUPPORT,DM_BIND_UNIFORM,DM_BIND_STORAGE,DM_BIND_UBO_POINTER,
       DM_BIND_DRIVER,DM_BIND_CONSTANT,DM_BIND_TEXTURE,DM_BIND_IMAGE,DM_BIND_COUNT,
       DM_BIND_INPUT_ATTACHMENT };
typedef struct { uint32_t binding,kind; int slot; uint32_t flags,type; int sampler_slot; } dm_shader_binding;
typedef struct {
    nvn_texdesc desc; uint64_t serial,address;
    int base_level,levels,base_layer,layers,format;
    int packaged; // Upstream: tex_t.has_layout && tex_t.mem; CPU-owned assets.
    uint8_t minf,magf,ws,wt,wr,compare,compare_func;
    float lod_min,lod_max,lod_bias,aniso,border[4];
} dm_compute_image;
typedef struct {
    const char *key; const uint32_t *code; size_t code_size;
    const dm_shader_binding *bindings; unsigned nbindings;
    const uint8_t *constants; size_t constants_size;
    uint32_t local[3]; int builtin;
} dm_compute_shader;
typedef struct {
    uint32_t binding; uint64_t address; size_t size;
    const void *host; int writable,zero; // zero: explicit 64 KiB missing/empty binding.
    const dm_compute_image *image;
} dm_compute_buffer;
const dm_compute_shader *dm_vk_compute_lookup(const char *key);
int dm_vk_compute(const dm_compute_shader *shader,const dm_compute_buffer *buffers,unsigned n,
                  uint32_t x,uint32_t y,uint32_t z);
typedef struct { int x,y,z,width,height,depth; } dm_image_region;
int dm_vk_image_buffer(const dm_compute_image *image,const dm_image_region *region,
                       uint64_t buffer,int64_t stride,int download);
int dm_vk_image_read_texels(const dm_compute_image *image,const dm_image_region *region,
                           void *destination,int64_t row_stride,int64_t image_stride);
int dm_vk_image_copy(const dm_compute_image *src,const dm_image_region *sr,
                     const dm_compute_image *dst,const dm_image_region *dr);
int dm_vk_image_clear(const dm_compute_image *image,const dm_image_region *region,const void *color);
int dm_vk_present_texture(const dm_compute_image *image,const int crop[4],uint64_t *ticket);
int dm_vk_present_wait(uint64_t ticket);
int dm_vk_color_attachment_clear(const dm_compute_image *image,const int rect[4],const float color[4]);
int dm_vk_depth_attachment_clear(const dm_compute_image *image,const int rect[4],float depth,int depth_mask,int stencil,int stencil_mask);
typedef struct dm_fuse_shader_info dm_fuse_shader_info;
typedef struct dm_graphics_shader {
    char key[17];uint32_t stage,fragment_map;
    uint32_t guarded_uniform_abi; // Archive ABI1: graphics push vec4Counts[32].
    const uint32_t *code;size_t code_size;
    const char *reflection;const uint8_t *constants;size_t constants_size;
    dm_shader_binding bindings[64];unsigned nbindings;
    const dm_fuse_shader_info *fuse;
    uint32_t fuse_mask; // Variant input bits; zero retains ordinary sampling.
} dm_graphics_shader;
const dm_graphics_shader *dm_vk_graphics_lookup(const char *key);
const dm_graphics_shader *dm_vk_graphics_fuse_variant(const dm_graphics_shader *base,uint32_t mask);
uint64_t dm_vk_graphics_generation(void);
