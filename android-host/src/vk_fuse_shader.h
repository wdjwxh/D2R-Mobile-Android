#pragma once
#include "vk_compute.h"

/* Facts describe the final Android SPIR-V, including its position Y sign.
 * Missing proof is -1. Guarded-UBO affine expressions are not exposed yet. */
typedef struct {
    int32_t pos_attr,w_one,xyz_sign[3],uv_attr[32];
    uint32_t may_discard,writes_sample_mask,writes_frag_depth,color_mask;
} dm_fuse_raster;
typedef struct {
    uint32_t binding_index,input_index,varying_mask;
} dm_fuse_input;
struct dm_fuse_shader_info {
    dm_fuse_raster raster;
    uint32_t ninputs,nvariants;
    dm_fuse_input inputs[8];
    /* All nonzero subset masks are present; mask N resides at variants[N-1].
     * Entries and their SPIR-V remain stable until graphics_catalog_close. */
    const dm_graphics_shader *variants;
};
