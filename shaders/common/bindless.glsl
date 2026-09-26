// shaders/common/bindless.glsl — GLSL view of the universal binding model.
//
// FROZEN CONTRACT (ADR-0002). Must match the RHI's single pipeline layout exactly:
//   set 0, binding 0 : combined image samplers (bindless array; alias by view type)
//   set 0, binding 1 : storage images           (bindless array; alias by format)
//   push constants   : <= 128 bytes, all stages
//   buffers          : buffer device address only (GL_EXT_buffer_reference) - declare
//                      buffer_reference blocks and place the REFERENCE TYPES directly in
//                      push constants / other buffers (no uint64 casts => no shaderInt64).
// Indices are DescriptorHandle::index() values written by the CPU. AE_INVALID_INDEX marks
// "no texture".
#ifndef AE_BINDLESS_GLSL
#define AE_BINDLESS_GLSL

#extension GL_EXT_nonuniform_qualifier : require
#extension GL_EXT_buffer_reference : require
#extension GL_EXT_buffer_reference2 : require
#extension GL_EXT_scalar_block_layout : require
#extension GL_EXT_samplerless_texture_functions : enable

#define AE_INVALID_INDEX 0xFFFFFFFFu

// ---- binding 0: sampled (combined image sampler), aliased by view type ----
layout(set = 0, binding = 0) uniform sampler2D            ae_tex2d[];
layout(set = 0, binding = 0) uniform samplerCube          ae_texcube[];
layout(set = 0, binding = 0) uniform sampler2DArray       ae_tex2darray[];
layout(set = 0, binding = 0) uniform sampler3D            ae_tex3d[];
layout(set = 0, binding = 0) uniform sampler2DShadow      ae_tex2dshadow[];      // compare sampler
layout(set = 0, binding = 0) uniform sampler2DArrayShadow ae_tex2darrayshadow[]; // compare sampler

// ---- binding 1: storage images, aliased by format ----
layout(set = 0, binding = 1, rgba16f) uniform image2D ae_img2d_rgba16f[];
layout(set = 0, binding = 1, rgba8)   uniform image2D ae_img2d_rgba8[];
layout(set = 0, binding = 1, r32f)    uniform image2D ae_img2d_r32f[];
layout(set = 0, binding = 1, rg16f)   uniform image2D ae_img2d_rg16f[];
layout(set = 0, binding = 1, r32ui)   uniform uimage2D ae_img2d_r32ui[];
layout(set = 0, binding = 1, rgba16f) uniform image2DArray ae_img2darray_rgba16f[];

// Convenience accessors (nonuniformEXT is required for divergent indices).
#define AE_TEX2D(i)       ae_tex2d[nonuniformEXT(i)]
#define AE_TEXCUBE(i)     ae_texcube[nonuniformEXT(i)]
#define AE_TEX2DARRAY(i)  ae_tex2darray[nonuniformEXT(i)]
#define AE_TEX3D(i)       ae_tex3d[nonuniformEXT(i)]
#define AE_SHADOW2D(i)    ae_tex2dshadow[nonuniformEXT(i)]
#define AE_SHADOWARRAY(i) ae_tex2darrayshadow[nonuniformEXT(i)]

// Sample with a factor fallback when no texture is bound.
vec4 ae_sample_or(uint index, vec2 uv, vec4 fallback) {
    return index == AE_INVALID_INDEX ? fallback : texture(AE_TEX2D(index), uv);
}

#endif // AE_BINDLESS_GLSL
