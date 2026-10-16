#pragma once
#include "vk_compute.h"
typedef struct {uint64_t address,size;uint32_t stride,divisor;} dm_vertex_stream;
typedef struct {uint32_t format,offset,stream;} dm_vertex_attribute;
typedef struct {int enabled,src_rgb,dst_rgb,src_alpha,dst_alpha,eq_rgb,eq_alpha;uint32_t mask;} dm_blend;
typedef struct {int fail,zfail,zpass,compare;uint32_t compare_mask,write_mask,reference;} dm_stencil;
typedef struct {
    const dm_graphics_shader *shader[2];const dm_compute_buffer *resources[2];
    dm_compute_image color[8];uint32_t color_mask;
    dm_compute_image depth;dm_stencil stencil[2];int depth_write,depth_compare;
    dm_vertex_stream streams[16];dm_vertex_attribute attributes[16];unsigned nattributes;
    dm_blend blend[8];float blend_constant[4],viewport[6];int scissor[4];
    int cull,front,polygon,logic,alpha,depth_test,stencil_test,has_depth;
    int raster_samples,alpha_to_coverage,depth_bias;float bias_factor,bias_units,bias_clamp;
    int primitive,index_type,count,first,base_vertex,base_instance,instances,indexed;
    uint64_t index_address;uint64_t *samples;
    uint64_t fuse_pass;uint32_t fuse_subpass; // Backend-owned pipeline identity; normal=0.
} dm_draw;
int dm_vk_draw(const dm_draw *draw);

typedef struct {
    dm_compute_image color[8],depth;uint32_t color_mask;int has_depth;
} dm_fuse_targets;
typedef struct {
    dm_fuse_targets targets[2];
    const dm_graphics_shader *q_shader;uint32_t q_mask;
    // Indexed by the shader facts' fixed input_index (not by descriptor binding).
    dm_compute_image inputs[8];
    // Only Q-private attachments proven unobserved by P may clear at pass begin.
    uint32_t q_clear_colors;float q_clear_rgba[8][4];
    int q_clear_depth,q_clear_stencil;float q_depth;uint32_t q_stencil;
} dm_fuse_plan;
// 0 accepted, 1 safe ordinary-path fallback, -1 actual failure. next/end are
// token scoped: an abandoned token must never close a subsequent ordinary pass.
// next requests Q; actual NextSubpass waits until Q draw preparation completes.
int dm_vk_fuse_begin(const dm_fuse_plan *plan,uint64_t *token);
int dm_vk_fuse_next(uint64_t token);
int dm_vk_fuse_end(uint64_t token);
