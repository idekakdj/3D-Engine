// aether/rhi/enums.h — backend-agnostic render enums (no Vk* leakage).
// FROZEN CONTRACT (ADR-0001 / blueprint §6). Vulkan-shaped but abstract.
#pragma once

#include "aether/core/types.h"

namespace aether::rhi {

enum class Format : u16 {
    Undefined = 0,
    R8Unorm, RG8Unorm, RGBA8Unorm, RGBA8Srgb, BGRA8Unorm, BGRA8Srgb,
    R16F, RG16F, RGBA16F,
    R32F, RG32F, RGB32F, RGBA32F,
    R32Uint, RG32Uint, RGBA32Uint,
    D32F, D24UnormS8, D32FS8,
    BC1Srgb, BC3Srgb, BC5Unorm, BC7Srgb, // block-compressed
};

enum class TextureType : u8 { Tex1D, Tex2D, Tex3D, Cube, Tex2DArray, CubeArray };

enum class TextureUsage : u32 {
    None          = 0,
    Sampled       = 1 << 0,
    Storage       = 1 << 1,
    ColorAttach   = 1 << 2,
    DepthAttach   = 1 << 3,
    TransferSrc   = 1 << 4,
    TransferDst   = 1 << 5,
};

enum class BufferUsage : u32 {
    None      = 0,
    Vertex    = 1 << 0,
    Index     = 1 << 1,
    Uniform   = 1 << 2,
    Storage   = 1 << 3,
    Indirect  = 1 << 4,
    TransferSrc = 1 << 5,
    TransferDst = 1 << 6,
};

enum class MemoryUsage : u8 { GpuOnly, CpuToGpu /*upload*/, GpuToCpu /*readback*/, CpuOnly };

enum class ShaderStage : u32 {
    Vertex   = 1 << 0,
    Fragment = 1 << 1,
    Compute  = 1 << 2,
    Geometry = 1 << 3,
    Task     = 1 << 4,
    Mesh     = 1 << 5,
    AllGraphics = Vertex | Fragment,
};

enum class PrimitiveTopology : u8 { TriangleList, TriangleStrip, LineList, LineStrip, PointList };
enum class CullMode : u8 { None, Front, Back };
enum class FrontFace : u8 { CounterClockwise, Clockwise };
enum class PolygonMode : u8 { Fill, Line, Point };
enum class CompareOp : u8 { Never, Less, Equal, LessEqual, Greater, NotEqual, GreaterEqual, Always };
enum class Filter : u8 { Nearest, Linear };
enum class MipmapMode : u8 { Nearest, Linear };
enum class AddressMode : u8 { Repeat, MirroredRepeat, ClampToEdge, ClampToBorder };
enum class BlendFactor : u8 { Zero, One, SrcAlpha, OneMinusSrcAlpha, DstAlpha, OneMinusDstAlpha, SrcColor, OneMinusSrcColor };
enum class BlendOp : u8 { Add, Subtract, ReverseSubtract, Min, Max };
enum class LoadOp : u8 { Load, Clear, DontCare };
enum class StoreOp : u8 { Store, DontCare };

// Resource state for barriers (synchronization2-shaped, abstract).
enum class ResourceState : u16 {
    Undefined = 0,
    General,
    ColorAttachment,
    DepthStencilAttachment,
    DepthStencilRead,
    ShaderRead,
    ShaderWrite,
    TransferSrc,
    TransferDst,
    Present,
    IndirectArgument,
};

// bit-flag helpers -----------------------------------------------------------
#define AE_RHI_FLAG_OPS(E)                                                                          \
    inline E operator|(E a, E b) { return static_cast<E>(static_cast<u32>(a) | static_cast<u32>(b)); } \
    inline E operator&(E a, E b) { return static_cast<E>(static_cast<u32>(a) & static_cast<u32>(b)); } \
    inline E& operator|=(E& a, E b) { a = a | b; return a; }                                        \
    inline bool any(E v) { return static_cast<u32>(v) != 0; }
AE_RHI_FLAG_OPS(TextureUsage)
AE_RHI_FLAG_OPS(BufferUsage)
AE_RHI_FLAG_OPS(ShaderStage)
#undef AE_RHI_FLAG_OPS

} // namespace aether::rhi
