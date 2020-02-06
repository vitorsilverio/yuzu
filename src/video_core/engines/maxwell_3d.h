// Copyright 2018 yuzu Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include <array>
#include <bitset>
#include <type_traits>
#include <unordered_map>
#include <vector>

#include "common/assert.h"
#include "common/bit_field.h"
#include "common/common_funcs.h"
#include "common/common_types.h"
#include "common/math_util.h"
#include "video_core/engines/const_buffer_engine_interface.h"
#include "video_core/engines/const_buffer_info.h"
#include "video_core/engines/engine_upload.h"
#include "video_core/engines/shader_type.h"
#include "video_core/gpu.h"
#include "video_core/macro_interpreter.h"
#include "video_core/textures/texture.h"

namespace Core {
class System;
}

namespace Tegra {
class MemoryManager;
}

namespace VideoCore {
class RasterizerInterface;
}

namespace Tegra::Engines {

/**
 * This Engine is known as GF100_3D. Documentation can be found in:
 * https://github.com/envytools/envytools/blob/master/rnndb/graph/gf100_3d.xml
 * https://cgit.freedesktop.org/mesa/mesa/tree/src/gallium/drivers/nouveau/nvc0/nvc0_3d.xml.h
 */

#define MAXWELL3D_REG_INDEX(field_name)                                                            \
    (offsetof(Tegra::Engines::Maxwell3D::Regs, field_name) / sizeof(u32))

class Maxwell3D final : public ConstBufferEngineInterface {
public:
    explicit Maxwell3D(Core::System& system, VideoCore::RasterizerInterface& rasterizer,
                       MemoryManager& memory_manager);
    ~Maxwell3D() = default;

    /// Register structure of the Maxwell3D engine.
    /// TODO(Subv): This structure will need to be made bigger as more registers are discovered.
    struct Regs {
        static constexpr std::size_t NUM_REGS = 0xE00;

        static constexpr std::size_t NumRenderTargets = 8;
        static constexpr std::size_t NumViewports = 16;
        static constexpr std::size_t NumCBData = 16;
        static constexpr std::size_t NumVertexArrays = 32;
        static constexpr std::size_t NumVertexAttributes = 32;
        static constexpr std::size_t NumVaryings = 31;
        static constexpr std::size_t NumImages = 8; // TODO(Rodrigo): Investigate this number
        static constexpr std::size_t NumClipDistances = 8;
        static constexpr std::size_t MaxShaderProgram = 6;
        static constexpr std::size_t MaxShaderStage = 5;
        // Maximum number of const buffers per shader stage.
        static constexpr std::size_t MaxConstBuffers = 18;
        static constexpr std::size_t MaxConstBufferSize = 0x10000;

        enum class QueryMode : u32 {
            Write = 0,
            Sync = 1,
            // TODO(Subv): It is currently unknown what the difference between method 2 and method 0
            // is.
            Write2 = 2,
        };

        enum class QueryUnit : u32 {
            VFetch = 1,
            VP = 2,
            Rast = 4,
            StrmOut = 5,
            GP = 6,
            ZCull = 7,
            Prop = 10,
            Crop = 15,
        };

        enum class QuerySelect : u32 {
            Zero = 0,
            TimeElapsed = 2,
            TransformFeedbackPrimitivesGenerated = 11,
            PrimitivesGenerated = 18,
            SamplesPassed = 21,
            TransformFeedbackUnknown = 26,
        };

        struct QueryCompare {
            u32 initial_sequence;
            u32 initial_mode;
            u32 unknown1;
            u32 unknown2;
            u32 current_sequence;
            u32 current_mode;
        };

        enum class QuerySyncCondition : u32 {
            NotEqual = 0,
            GreaterThan = 1,
        };

        enum class ConditionMode : u32 {
            Never = 0,
            Always = 1,
            ResNonZero = 2,
            Equal = 3,
            NotEqual = 4,
        };

        enum class ShaderProgram : u32 {
            VertexA = 0,
            VertexB = 1,
            TesselationControl = 2,
            TesselationEval = 3,
            Geometry = 4,
            Fragment = 5,
        };

        struct VertexAttribute {
            enum class Size : u32 {
                Invalid = 0x0,
                Size_32_32_32_32 = 0x01,
                Size_32_32_32 = 0x02,
                Size_16_16_16_16 = 0x03,
                Size_32_32 = 0x04,
                Size_16_16_16 = 0x05,
                Size_8_8_8_8 = 0x0a,
                Size_16_16 = 0x0f,
                Size_32 = 0x12,
                Size_8_8_8 = 0x13,
                Size_8_8 = 0x18,
                Size_16 = 0x1b,
                Size_8 = 0x1d,
                Size_10_10_10_2 = 0x30,
                Size_11_11_10 = 0x31,
            };

            enum class Type : u32 {
                SignedNorm = 1,
                UnsignedNorm = 2,
                SignedInt = 3,
                UnsignedInt = 4,
                UnsignedScaled = 5,
                SignedScaled = 6,
                Float = 7,
            };

            union {
                BitField<0, 5, u32> buffer;
                BitField<6, 1, u32> constant;
                BitField<7, 14, u32> offset;
                BitField<21, 6, Size> size;
                BitField<27, 3, Type> type;
                BitField<31, 1, u32> bgra;
                u32 hex;
            };

            u32 ComponentCount() const {
                switch (size) {
                case Size::Size_32_32_32_32:
                    return 4;
                case Size::Size_32_32_32:
                    return 3;
                case Size::Size_16_16_16_16:
                    return 4;
                case Size::Size_32_32:
                    return 2;
                case Size::Size_16_16_16:
                    return 3;
                case Size::Size_8_8_8_8:
                    return 4;
                case Size::Size_16_16:
                    return 2;
                case Size::Size_32:
                    return 1;
                case Size::Size_8_8_8:
                    return 3;
                case Size::Size_8_8:
                    return 2;
                case Size::Size_16:
                    return 1;
                case Size::Size_8:
                    return 1;
                case Size::Size_10_10_10_2:
                    return 4;
                case Size::Size_11_11_10:
                    return 3;
                default:
                    UNREACHABLE();
                    return 1;
                }
            }

            u32 SizeInBytes() const {
                switch (size) {
                case Size::Size_32_32_32_32:
                    return 16;
                case Size::Size_32_32_32:
                    return 12;
                case Size::Size_16_16_16_16:
                    return 8;
                case Size::Size_32_32:
                    return 8;
                case Size::Size_16_16_16:
                    return 6;
                case Size::Size_8_8_8_8:
                    return 4;
                case Size::Size_16_16:
                    return 4;
                case Size::Size_32:
                    return 4;
                case Size::Size_8_8_8:
                    return 3;
                case Size::Size_8_8:
                    return 2;
                case Size::Size_16:
                    return 2;
                case Size::Size_8:
                    return 1;
                case Size::Size_10_10_10_2:
                    return 4;
                case Size::Size_11_11_10:
                    return 4;
                default:
                    UNREACHABLE();
                }
            }

            std::string SizeString() const {
                switch (size) {
                case Size::Size_32_32_32_32:
                    return "32_32_32_32";
                case Size::Size_32_32_32:
                    return "32_32_32";
                case Size::Size_16_16_16_16:
                    return "16_16_16_16";
                case Size::Size_32_32:
                    return "32_32";
                case Size::Size_16_16_16:
                    return "16_16_16";
                case Size::Size_8_8_8_8:
                    return "8_8_8_8";
                case Size::Size_16_16:
                    return "16_16";
                case Size::Size_32:
                    return "32";
                case Size::Size_8_8_8:
                    return "8_8_8";
                case Size::Size_8_8:
                    return "8_8";
                case Size::Size_16:
                    return "16";
                case Size::Size_8:
                    return "8";
                case Size::Size_10_10_10_2:
                    return "10_10_10_2";
                case Size::Size_11_11_10:
                    return "11_11_10";
                default:
                    UNREACHABLE();
                    return {};
                }
            }

            std::string TypeString() const {
                switch (type) {
                case Type::SignedNorm:
                    return "SNORM";
                case Type::UnsignedNorm:
                    return "UNORM";
                case Type::SignedInt:
                    return "SINT";
                case Type::UnsignedInt:
                    return "UINT";
                case Type::UnsignedScaled:
                    return "USCALED";
                case Type::SignedScaled:
                    return "SSCALED";
                case Type::Float:
                    return "FLOAT";
                }
                UNREACHABLE();
                return {};
            }

            bool IsNormalized() const {
                return (type == Type::SignedNorm) || (type == Type::UnsignedNorm);
            }

            bool IsValid() const {
                return size != Size::Invalid;
            }

            bool operator<(const VertexAttribute& other) const {
                return hex < other.hex;
            }
        };

        enum class DepthMode : u32 {
            MinusOneToOne = 0,
            ZeroToOne = 1,
        };

        enum class PrimitiveTopology : u32 {
            Points = 0x0,
            Lines = 0x1,
            LineLoop = 0x2,
            LineStrip = 0x3,
            Triangles = 0x4,
            TriangleStrip = 0x5,
            TriangleFan = 0x6,
            Quads = 0x7,
            QuadStrip = 0x8,
            Polygon = 0x9,
            LinesAdjacency = 0xa,
            LineStripAdjacency = 0xb,
            TrianglesAdjacency = 0xc,
            TriangleStripAdjacency = 0xd,
            Patches = 0xe,
        };

        enum class IndexFormat : u32 {
            UnsignedByte = 0x0,
            UnsignedShort = 0x1,
            UnsignedInt = 0x2,
        };

        enum class ComparisonOp : u32 {
            // These values are used by Nouveau and most games, they correspond to the OpenGL token
            // values for these operations.
            Never = 0x200,
            Less = 0x201,
            Equal = 0x202,
            LessEqual = 0x203,
            Greater = 0x204,
            NotEqual = 0x205,
            GreaterEqual = 0x206,
            Always = 0x207,

            // These values are used by some games, they seem to be NV04 values.
            NeverOld = 1,
            LessOld = 2,
            EqualOld = 3,
            LessEqualOld = 4,
            GreaterOld = 5,
            NotEqualOld = 6,
            GreaterEqualOld = 7,
            AlwaysOld = 8,
        };

        enum class LogicOperation : u32 {
            Clear = 0x1500,
            And = 0x1501,
            AndReverse = 0x1502,
            Copy = 0x1503,
            AndInverted = 0x1504,
            NoOp = 0x1505,
            Xor = 0x1506,
            Or = 0x1507,
            Nor = 0x1508,
            Equiv = 0x1509,
            Invert = 0x150A,
            OrReverse = 0x150B,
            CopyInverted = 0x150C,
            OrInverted = 0x150D,
            Nand = 0x150E,
            Set = 0x150F,
        };

        enum class StencilOp : u32 {
            Keep = 1,
            Zero = 2,
            Replace = 3,
            Incr = 4,
            Decr = 5,
            Invert = 6,
            IncrWrap = 7,
            DecrWrap = 8,
            KeepOGL = 0x1E00,
            ZeroOGL = 0,
            ReplaceOGL = 0x1E01,
            IncrOGL = 0x1E02,
            DecrOGL = 0x1E03,
            InvertOGL = 0x150A,
            IncrWrapOGL = 0x8507,
            DecrWrapOGL = 0x8508,
        };

        enum class MemoryLayout : u32 {
            Linear = 0,
            BlockLinear = 1,
        };

        enum class InvMemoryLayout : u32 {
            BlockLinear = 0,
            Linear = 1,
        };

        struct Cull {
            enum class FrontFace : u32 {
                ClockWise = 0x0900,
                CounterClockWise = 0x0901,
            };

            enum class CullFace : u32 {
                Front = 0x0404,
                Back = 0x0405,
                FrontAndBack = 0x0408,
            };

            u32 enabled;
            FrontFace front_face;
            CullFace cull_face;
        };

        struct Blend {
            enum class Equation : u32 {
                Add = 1,
                Subtract = 2,
                ReverseSubtract = 3,
                Min = 4,
                Max = 5,

                // These values are used by Nouveau and some games.
                AddGL = 0x8006,
                SubtractGL = 0x8007,
                ReverseSubtractGL = 0x8008,
                MinGL = 0x800a,
                MaxGL = 0x800b
            };

            enum class Factor : u32 {
                Zero = 0x1,
                One = 0x2,
                SourceColor = 0x3,
                OneMinusSourceColor = 0x4,
                SourceAlpha = 0x5,
                OneMinusSourceAlpha = 0x6,
                DestAlpha = 0x7,
                OneMinusDestAlpha = 0x8,
                DestColor = 0x9,
                OneMinusDestColor = 0xa,
                SourceAlphaSaturate = 0xb,
                Source1Color = 0x10,
                OneMinusSource1Color = 0x11,
                Source1Alpha = 0x12,
                OneMinusSource1Alpha = 0x13,
                ConstantColor = 0x61,
                OneMinusConstantColor = 0x62,
                ConstantAlpha = 0x63,
                OneMinusConstantAlpha = 0x64,

                // These values are used by Nouveau and some games.
                ZeroGL = 0x4000,
                OneGL = 0x4001,
                SourceColorGL = 0x4300,
                OneMinusSourceColorGL = 0x4301,
                SourceAlphaGL = 0x4302,
                OneMinusSourceAlphaGL = 0x4303,
                DestAlphaGL = 0x4304,
                OneMinusDestAlphaGL = 0x4305,
                DestColorGL = 0x4306,
                OneMinusDestColorGL = 0x4307,
                SourceAlphaSaturateGL = 0x4308,
                ConstantColorGL = 0xc001,
                OneMinusConstantColorGL = 0xc002,
                ConstantAlphaGL = 0xc003,
                OneMinusConstantAlphaGL = 0xc004,
                Source1ColorGL = 0xc900,
                OneMinusSource1ColorGL = 0xc901,
                Source1AlphaGL = 0xc902,
                OneMinusSource1AlphaGL = 0xc903,
            };

            u32 separate_alpha;
            Equation equation_rgb;
            Factor factor_source_rgb;
            Factor factor_dest_rgb;
            Equation equation_a;
            Factor factor_source_a;
            Factor factor_dest_a;
            INSERT_UNION_PADDING_WORDS(1);
        };

        enum class TessellationPrimitive : u32 {
            Isolines = 0,
            Triangles = 1,
            Quads = 2,
        };

        enum class TessellationSpacing : u32 {
            Equal = 0,
            FractionalOdd = 1,
            FractionalEven = 2,
        };

        struct RenderTargetConfig {
            u32 address_high;
            u32 address_low;
            u32 width;
            u32 height;
            Tegra::RenderTargetFormat format;
            union {
                BitField<0, 3, u32> block_width;
                BitField<4, 3, u32> block_height;
                BitField<8, 3, u32> block_depth;
                BitField<12, 1, InvMemoryLayout> type;
            } memory_layout;
            union {
                BitField<0, 16, u32> array_mode;
                BitField<16, 1, u32> volume;
            };
            u32 layer_stride;
            u32 base_layer;
            INSERT_UNION_PADDING_WORDS(7);

            GPUVAddr Address() const {
                return static_cast<GPUVAddr>((static_cast<GPUVAddr>(address_high) << 32) |
                                             address_low);
            }
        };

        struct ColorMask {
            union {
                u32 raw;
                BitField<0, 4, u32> R;
                BitField<4, 4, u32> G;
                BitField<8, 4, u32> B;
                BitField<12, 4, u32> A;
            };
        };

        struct ViewportTransform {
            f32 scale_x;
            f32 scale_y;
            f32 scale_z;
            f32 translate_x;
            f32 translate_y;
            f32 translate_z;
            INSERT_UNION_PADDING_WORDS(2);

            Common::Rectangle<s32> GetRect() const {
                return {
                    GetX(),               // left
                    GetY() + GetHeight(), // top
                    GetX() + GetWidth(),  // right
                    GetY()                // bottom
                };
            };

            s32 GetX() const {
                return static_cast<s32>(std::max(0.0f, translate_x - std::fabs(scale_x)));
            }

            s32 GetY() const {
                return static_cast<s32>(std::max(0.0f, translate_y - std::fabs(scale_y)));
            }

            s32 GetWidth() const {
                return static_cast<s32>(translate_x + std::fabs(scale_x)) - GetX();
            }

            s32 GetHeight() const {
                return static_cast<s32>(translate_y + std::fabs(scale_y)) - GetY();
            }
        };

        struct ScissorTest {
            u32 enable;
            union {
                BitField<0, 16, u32> min_x;
                BitField<16, 16, u32> max_x;
            };
            union {
                BitField<0, 16, u32> min_y;
                BitField<16, 16, u32> max_y;
            };
            u32 fill;
        };

        struct ViewPort {
            union {
                BitField<0, 16, u32> x;
                BitField<16, 16, u32> width;
            };
            union {
                BitField<0, 16, u32> y;
                BitField<16, 16, u32> height;
            };
            float depth_range_near;
            float depth_range_far;
        };

        bool IsShaderConfigEnabled(std::size_t index) const {
            // The VertexB is always enabled.
            if (index == static_cast<std::size_t>(Regs::ShaderProgram::VertexB)) {
                return true;
            }
            return shader_config[index].enable != 0;
        }

        union {
            struct {
                INSERT_UNION_PADDING_WORDS(0x45);

                struct {
                    u32 upload_address;
                    u32 data;
                    u32 entry;
                    u32 bind;
                } macros;

                INSERT_UNION_PADDING_WORDS(0x17);

                Upload::Registers upload;
                struct {
                    union {
                        BitField<0, 1, u32> linear;
                    };
                } exec_upload;

                u32 data_upload;

                INSERT_UNION_PADDING_WORDS(0x44);

                struct {
                    union {
                        BitField<0, 16, u32> sync_point;
                        BitField<16, 1, u32> unknown;
                        BitField<20, 1, u32> increment;
                    };
                } sync_info;

                INSERT_UNION_PADDING_WORDS(0x15);

                union {
                    BitField<0, 2, TessellationPrimitive> prim;
                    BitField<4, 2, TessellationSpacing> spacing;
                    BitField<8, 1, u32> cw;
                    BitField<9, 1, u32> connected;
                } tess_mode;

                std::array<f32, 4> tess_level_outer;
                std::array<f32, 2> tess_level_inner;

                INSERT_UNION_PADDING_WORDS(0x10);

                u32 rasterize_enable;

                INSERT_UNION_PADDING_WORDS(0xF1);

                u32 tfb_enabled;

                INSERT_UNION_PADDING_WORDS(0x2E);

                std::array<RenderTargetConfig, NumRenderTargets> rt;

                std::array<ViewportTransform, NumViewports> viewport_transform;

                std::array<ViewPort, NumViewports> viewports;

                INSERT_UNION_PADDING_WORDS(0x1D);

                struct {
                    u32 first;
                    u32 count;
                } vertex_buffer;

                DepthMode depth_mode;

                float clear_color[4];
                float clear_depth;

                INSERT_UNION_PADDING_WORDS(0x3);

                s32 clear_stencil;

                INSERT_UNION_PADDING_WORDS(0x7);

                u32 polygon_offset_point_enable;
                u32 polygon_offset_line_enable;
                u32 polygon_offset_fill_enable;

                u32 patch_vertices;

                INSERT_UNION_PADDING_WORDS(0xC);

                std::array<ScissorTest, NumViewports> scissor_test;

                INSERT_UNION_PADDING_WORDS(0x15);

                s32 stencil_back_func_ref;
                u32 stencil_back_mask;
                u32 stencil_back_func_mask;

                INSERT_UNION_PADDING_WORDS(0xC);

                u32 color_mask_common;

                INSERT_UNION_PADDING_WORDS(0x2);

                f32 depth_bounds[2];

                INSERT_UNION_PADDING_WORDS(0x2);

                u32 rt_separate_frag_data;

                INSERT_UNION_PADDING_WORDS(0xC);

                struct {
                    u32 address_high;
                    u32 address_low;
                    Tegra::DepthFormat format;
                    union {
                        BitField<0, 4, u32> block_width;
                        BitField<4, 4, u32> block_height;
                        BitField<8, 4, u32> block_depth;
                        BitField<20, 1, InvMemoryLayout> type;
                    } memory_layout;
                    u32 layer_stride;

                    GPUVAddr Address() const {
                        return static_cast<GPUVAddr>((static_cast<GPUVAddr>(address_high) << 32) |
                                                     address_low);
                    }
                } zeta;

                INSERT_UNION_PADDING_WORDS(0x41);

                union {
                    BitField<0, 4, u32> stencil;
                    BitField<4, 4, u32> unknown;
                    BitField<8, 4, u32> scissor;
                    BitField<12, 4, u32> viewport;
                } clear_flags;

                INSERT_UNION_PADDING_WORDS(0x19);

                std::array<VertexAttribute, NumVertexAttributes> vertex_attrib_format;

                INSERT_UNION_PADDING_WORDS(0xF);

                struct {
                    union {
                        BitField<0, 4, u32> count;
                        BitField<4, 3, u32> map_0;
                        BitField<7, 3, u32> map_1;
                        BitField<10, 3, u32> map_2;
                        BitField<13, 3, u32> map_3;
                        BitField<16, 3, u32> map_4;
                        BitField<19, 3, u32> map_5;
                        BitField<22, 3, u32> map_6;
                        BitField<25, 3, u32> map_7;
                    };

                    u32 GetMap(std::size_t index) const {
                        const std::array<u32, NumRenderTargets> maps{map_0, map_1, map_2, map_3,
                                                                     map_4, map_5, map_6, map_7};
                        ASSERT(index < maps.size());
                        return maps[index];
                    }
                } rt_control;

                INSERT_UNION_PADDING_WORDS(0x2);

                u32 zeta_width;
                u32 zeta_height;

                INSERT_UNION_PADDING_WORDS(0x27);

                u32 depth_test_enable;

                INSERT_UNION_PADDING_WORDS(0x5);

                u32 independent_blend_enable;

                u32 depth_write_enabled;

                u32 alpha_test_enabled;

                INSERT_UNION_PADDING_WORDS(0x6);

                u32 d3d_cull_mode;

                ComparisonOp depth_test_func;
                float alpha_test_ref;
                ComparisonOp alpha_test_func;
                u32 draw_tfb_stride;
                struct {
                    float r;
                    float g;
                    float b;
                    float a;
                } blend_color;
                INSERT_UNION_PADDING_WORDS(0x4);

                struct {
                    u32 separate_alpha;
                    Blend::Equation equation_rgb;
                    Blend::Factor factor_source_rgb;
                    Blend::Factor factor_dest_rgb;
                    Blend::Equation equation_a;
                    Blend::Factor factor_source_a;
                    INSERT_UNION_PADDING_WORDS(1);
                    Blend::Factor factor_dest_a;

                    u32 enable_common;
                    u32 enable[NumRenderTargets];
                } blend;

                u32 stencil_enable;
                StencilOp stencil_front_op_fail;
                StencilOp stencil_front_op_zfail;
                StencilOp stencil_front_op_zpass;
                ComparisonOp stencil_front_func_func;
                s32 stencil_front_func_ref;
                u32 stencil_front_func_mask;
                u32 stencil_front_mask;

                INSERT_UNION_PADDING_WORDS(0x2);

                u32 frag_color_clamp;

                union {
                    BitField<0, 1, u32> y_negate;
                    BitField<4, 1, u32> triangle_rast_flip;
                } screen_y_control;

                INSERT_UNION_PADDING_WORDS(0x21);

                u32 vb_element_base;
                u32 vb_base_instance;

                INSERT_UNION_PADDING_WORDS(0x35);

                union {
                    BitField<0, 1, u32> c0;
                    BitField<1, 1, u32> c1;
                    BitField<2, 1, u32> c2;
                    BitField<3, 1, u32> c3;
                    BitField<4, 1, u32> c4;
                    BitField<5, 1, u32> c5;
                    BitField<6, 1, u32> c6;
                    BitField<7, 1, u32> c7;
                } clip_distance_enabled;

                INSERT_UNION_PADDING_WORDS(0x1);

                float point_size;

                INSERT_UNION_PADDING_WORDS(0x1);

                u32 point_sprite_enable;

                INSERT_UNION_PADDING_WORDS(0x5);

                u32 zeta_enable;

                union {
                    BitField<0, 1, u32> alpha_to_coverage;
                    BitField<4, 1, u32> alpha_to_one;
                } multisample_control;

                INSERT_UNION_PADDING_WORDS(0x4);

                struct {
                    u32 address_high;
                    u32 address_low;
                    ConditionMode mode;

                    GPUVAddr Address() const {
                        return static_cast<GPUVAddr>((static_cast<GPUVAddr>(address_high) << 32) |
                                                     address_low);
                    }
                } condition;

                struct {
                    u32 tsc_address_high;
                    u32 tsc_address_low;
                    u32 tsc_limit;

                    GPUVAddr TSCAddress() const {
                        return static_cast<GPUVAddr>(
                            (static_cast<GPUVAddr>(tsc_address_high) << 32) | tsc_address_low);
                    }
                } tsc;

                INSERT_UNION_PADDING_WORDS(0x1);

                float polygon_offset_factor;

                INSERT_UNION_PADDING_WORDS(0x1);

                struct {
                    u32 tic_address_high;
                    u32 tic_address_low;
                    u32 tic_limit;

                    GPUVAddr TICAddress() const {
                        return static_cast<GPUVAddr>(
                            (static_cast<GPUVAddr>(tic_address_high) << 32) | tic_address_low);
                    }
                } tic;

                INSERT_UNION_PADDING_WORDS(0x5);

                u32 stencil_two_side_enable;
                StencilOp stencil_back_op_fail;
                StencilOp stencil_back_op_zfail;
                StencilOp stencil_back_op_zpass;
                ComparisonOp stencil_back_func_func;

                INSERT_UNION_PADDING_WORDS(0x4);

                u32 framebuffer_srgb;

                float polygon_offset_units;

                INSERT_UNION_PADDING_WORDS(0x11);

                union {
                    BitField<2, 1, u32> coord_origin;
                    BitField<3, 10, u32> enable;
                } point_coord_replace;

                struct {
                    u32 code_address_high;
                    u32 code_address_low;

                    GPUVAddr CodeAddress() const {
                        return static_cast<GPUVAddr>(
                            (static_cast<GPUVAddr>(code_address_high) << 32) | code_address_low);
                    }
                } code_address;
                INSERT_UNION_PADDING_WORDS(1);

                struct {
                    u32 vertex_end_gl;
                    union {
                        u32 vertex_begin_gl;
                        BitField<0, 16, PrimitiveTopology> topology;
                        BitField<26, 1, u32> instance_next;
                        BitField<27, 1, u32> instance_cont;
                    };
                } draw;

                INSERT_UNION_PADDING_WORDS(0xA);

                struct {
                    u32 enabled;
                    u32 index;
                } primitive_restart;

                INSERT_UNION_PADDING_WORDS(0x5F);

                struct {
                    u32 start_addr_high;
                    u32 start_addr_low;
                    u32 end_addr_high;
                    u32 end_addr_low;
                    IndexFormat format;
                    u32 first;
                    u32 count;

                    unsigned FormatSizeInBytes() const {
                        switch (format) {
                        case IndexFormat::UnsignedByte:
                            return 1;
                        case IndexFormat::UnsignedShort:
                            return 2;
                        case IndexFormat::UnsignedInt:
                            return 4;
                        }
                        UNREACHABLE();
                        return 1;
                    }

                    GPUVAddr StartAddress() const {
                        return static_cast<GPUVAddr>(
                            (static_cast<GPUVAddr>(start_addr_high) << 32) | start_addr_low);
                    }

                    GPUVAddr EndAddress() const {
                        return static_cast<GPUVAddr>((static_cast<GPUVAddr>(end_addr_high) << 32) |
                                                     end_addr_low);
                    }

                    /// Adjust the index buffer offset so it points to the first desired index.
                    GPUVAddr IndexStart() const {
                        return StartAddress() + static_cast<size_t>(first) *
                                                    static_cast<size_t>(FormatSizeInBytes());
                    }
                } index_array;

                INSERT_UNION_PADDING_WORDS(0x7);

                INSERT_UNION_PADDING_WORDS(0x1F);

                float polygon_offset_clamp;

                struct {
                    u32 is_instanced[NumVertexArrays];

                    /// Returns whether the vertex array specified by index is supposed to be
                    /// accessed per instance or not.
                    bool IsInstancingEnabled(u32 index) const {
                        return is_instanced[index];
                    }
                } instanced_arrays;

                INSERT_UNION_PADDING_WORDS(0x4);

                union {
                    BitField<0, 1, u32> enable;
                    BitField<4, 8, u32> unk4;
                } vp_point_size;

                INSERT_UNION_PADDING_WORDS(1);

                Cull cull;

                u32 pixel_center_integer;

                INSERT_UNION_PADDING_WORDS(0x1);

                u32 viewport_transform_enabled;

                INSERT_UNION_PADDING_WORDS(0x3);

                union {
                    BitField<0, 1, u32> depth_range_0_1;
                    BitField<3, 1, u32> depth_clamp_near;
                    BitField<4, 1, u32> depth_clamp_far;
                } view_volume_clip_control;

                INSERT_UNION_PADDING_WORDS(0x1F);

                u32 depth_bounds_enable;

                INSERT_UNION_PADDING_WORDS(1);

                struct {
                    u32 enable;
                    LogicOperation operation;
                } logic_op;

                INSERT_UNION_PADDING_WORDS(0x1);

                union {
                    u32 raw;
                    BitField<0, 1, u32> Z;
                    BitField<1, 1, u32> S;
                    BitField<2, 1, u32> R;
                    BitField<3, 1, u32> G;
                    BitField<4, 1, u32> B;
                    BitField<5, 1, u32> A;
                    BitField<6, 4, u32> RT;
                    BitField<10, 11, u32> layer;
                } clear_buffers;
                INSERT_UNION_PADDING_WORDS(0xB);
                std::array<ColorMask, NumRenderTargets> color_mask;
                INSERT_UNION_PADDING_WORDS(0x38);

                struct {
                    u32 query_address_high;
                    u32 query_address_low;
                    u32 query_sequence;
                    union {
                        u32 raw;
                        BitField<0, 2, QueryMode> mode;
                        BitField<4, 1, u32> fence;
                        BitField<12, 4, QueryUnit> unit;
                        BitField<16, 1, QuerySyncCondition> sync_cond;
                        BitField<23, 5, QuerySelect> select;
                        BitField<28, 1, u32> short_query;
                    } query_get;

                    GPUVAddr QueryAddress() const {
                        return static_cast<GPUVAddr>(
                            (static_cast<GPUVAddr>(query_address_high) << 32) | query_address_low);
                    }
                } query;

                INSERT_UNION_PADDING_WORDS(0x3C);

                struct {
                    union {
                        BitField<0, 12, u32> stride;
                        BitField<12, 1, u32> enable;
                    };
                    u32 start_high;
                    u32 start_low;
                    u32 divisor;

                    GPUVAddr StartAddress() const {
                        return static_cast<GPUVAddr>((static_cast<GPUVAddr>(start_high) << 32) |
                                                     start_low);
                    }

                    bool IsEnabled() const {
                        return enable != 0 && StartAddress() != 0;
                    }

                } vertex_array[NumVertexArrays];

                Blend independent_blend[NumRenderTargets];

                struct {
                    u32 limit_high;
                    u32 limit_low;

                    GPUVAddr LimitAddress() const {
                        return static_cast<GPUVAddr>((static_cast<GPUVAddr>(limit_high) << 32) |
                                                     limit_low);
                    }
                } vertex_array_limit[NumVertexArrays];

                struct {
                    union {
                        BitField<0, 1, u32> enable;
                        BitField<4, 4, ShaderProgram> program;
                    };
                    u32 offset;
                    INSERT_UNION_PADDING_WORDS(14);
                } shader_config[MaxShaderProgram];

                INSERT_UNION_PADDING_WORDS(0x60);

                u32 firmware[0x20];

                struct {
                    u32 cb_size;
                    u32 cb_address_high;
                    u32 cb_address_low;
                    u32 cb_pos;
                    u32 cb_data[NumCBData];

                    GPUVAddr BufferAddress() const {
                        return static_cast<GPUVAddr>(
                            (static_cast<GPUVAddr>(cb_address_high) << 32) | cb_address_low);
                    }
                } const_buffer;

                INSERT_UNION_PADDING_WORDS(0x10);

                struct {
                    union {
                        u32 raw_config;
                        BitField<0, 1, u32> valid;
                        BitField<4, 5, u32> index;
                    };
                    INSERT_UNION_PADDING_WORDS(7);
                } cb_bind[MaxShaderStage];

                INSERT_UNION_PADDING_WORDS(0x56);

                u32 tex_cb_index;

                INSERT_UNION_PADDING_WORDS(0x395);

                struct {
                    /// Compressed address of a buffer that holds information about bound SSBOs.
                    /// This address is usually bound to c0 in the shaders.
                    u32 buffer_address;

                    GPUVAddr BufferAddress() const {
                        return static_cast<GPUVAddr>(buffer_address) << 8;
                    }
                } ssbo_info;

                INSERT_UNION_PADDING_WORDS(0x11);

                struct {
                    u32 address[MaxShaderStage];
                    u32 size[MaxShaderStage];
                } tex_info_buffers;

                INSERT_UNION_PADDING_WORDS(0xCC);
            };
            std::array<u32, NUM_REGS> reg_array;
        };
    } regs{};

    static_assert(sizeof(Regs) == Regs::NUM_REGS * sizeof(u32), "Maxwell3D Regs has wrong size");
    static_assert(std::is_trivially_copyable_v<Regs>, "Maxwell3D Regs must be trivially copyable");

    struct State {
        struct ShaderStageInfo {
            std::array<ConstBufferInfo, Regs::MaxConstBuffers> const_buffers;
        };

        std::array<ShaderStageInfo, Regs::MaxShaderStage> shader_stages;
        u32 current_instance = 0; ///< Current instance to be used to simulate instanced rendering.
    };

    State state{};

    struct DirtyRegs {
        static constexpr std::size_t NUM_REGS = 256;
        static_assert(NUM_REGS - 1 <= std::numeric_limits<u8>::max());

        union {
            struct {
                bool null_dirty;

                // Vertex Attributes
                bool vertex_attrib_format;

                // Vertex Arrays
                std::array<bool, 32> vertex_array;

                bool vertex_array_buffers;

                // Vertex Instances
                std::array<bool, 32> vertex_instance;

                bool vertex_instances;

                // Render Targets
                std::array<bool, 8> render_target;
                bool depth_buffer;

                bool render_settings;

                // Shaders
                bool shaders;

                // Rasterizer State
                bool viewport;
                bool clip_coefficient;
                bool cull_mode;
                bool primitive_restart;
                bool depth_test;
                bool stencil_test;
                bool blend_state;
                bool scissor_test;
                bool transform_feedback;
                bool color_mask;
                bool polygon_offset;
                bool depth_bounds_values;

                // Complementary
                bool viewport_transform;
                bool screen_y_control;

                bool memory_general;
            };
            std::array<bool, NUM_REGS> regs;
        };

        void ResetVertexArrays() {
            vertex_array.fill(true);
            vertex_array_buffers = true;
        }

        void ResetRenderTargets() {
            depth_buffer = true;
            render_target.fill(true);
            render_settings = true;
        }

        void OnMemoryWrite() {
            shaders = true;
            memory_general = true;
            ResetRenderTargets();
            ResetVertexArrays();
        }

    } dirty{};

    /// Reads a register value located at the input method address
    u32 GetRegisterValue(u32 method) const;

    /// Write the value to the register identified by method.
    void CallMethod(const GPU::MethodCall& method_call);

    /// Write the value to the register identified by method.
    void CallMethodFromMME(const GPU::MethodCall& method_call);

    void FlushMMEInlineDraw();

    /// Given a texture handle, returns the TSC and TIC entries.
    Texture::FullTextureInfo GetTextureInfo(Texture::TextureHandle tex_handle) const;

    /// Returns the texture information for a specific texture in a specific shader stage.
    Texture::FullTextureInfo GetStageTexture(ShaderType stage, std::size_t offset) const;

    u32 AccessConstBuffer32(ShaderType stage, u64 const_buffer, u64 offset) const override;

    SamplerDescriptor AccessBoundSampler(ShaderType stage, u64 offset) const override;

    SamplerDescriptor AccessBindlessSampler(ShaderType stage, u64 const_buffer,
                                            u64 offset) const override;

    u32 GetBoundBuffer() const override {
        return regs.tex_cb_index;
    }

    VideoCore::GuestDriverProfile& AccessGuestDriverProfile() override;

    const VideoCore::GuestDriverProfile& AccessGuestDriverProfile() const override;

    /// Memory for macro code - it's undetermined how big this is, however 1MB is much larger than
    /// we've seen used.
    using MacroMemory = std::array<u32, 0x40000>;

    /// Gets a reference to macro memory.
    const MacroMemory& GetMacroMemory() const {
        return macro_memory;
    }

    bool ShouldExecute() const {
        return execute_on;
    }

    enum class MMEDrawMode : u32 {
        Undefined,
        Array,
        Indexed,
    };

    struct MMEDrawState {
        MMEDrawMode current_mode{MMEDrawMode::Undefined};
        u32 current_count{};
        u32 instance_count{};
        bool instance_mode{};
        bool gl_begin_consume{};
        u32 gl_end_count{};
    } mme_draw;

private:
    void InitializeRegisterDefaults();

    Core::System& system;

    VideoCore::RasterizerInterface& rasterizer;

    MemoryManager& memory_manager;

    /// Start offsets of each macro in macro_memory
    std::array<u32, 0x80> macro_positions = {};

    std::array<bool, Regs::NUM_REGS> mme_inline{};

    /// Memory for macro code
    MacroMemory macro_memory;

    /// Macro method that is currently being executed / being fed parameters.
    u32 executing_macro = 0;
    /// Parameters that have been submitted to the macro call so far.
    std::vector<u32> macro_params;

    /// Interpreter for the macro codes uploaded to the GPU.
    MacroInterpreter macro_interpreter;

    static constexpr u32 null_cb_data = 0xFFFFFFFF;
    struct {
        std::array<std::array<u32, 0x4000>, 16> buffer;
        u32 current{null_cb_data};
        u32 id{null_cb_data};
        u32 start_pos{};
        u32 counter{};
    } cb_data_state;

    Upload::State upload_state;

    bool execute_on{true};

    std::array<u8, Regs::NUM_REGS> dirty_pointers{};

    /// Retrieves information about a specific TIC entry from the TIC buffer.
    Texture::TICEntry GetTICEntry(u32 tic_index) const;

    /// Retrieves information about a specific TSC entry from the TSC buffer.
    Texture::TSCEntry GetTSCEntry(u32 tsc_index) const;

    void InitDirtySettings();

    /**
     * Call a macro on this engine.
     * @param method Method to call
     * @param num_parameters Number of arguments
     * @param parameters Arguments to the method call
     */
    void CallMacroMethod(u32 method, std::size_t num_parameters, const u32* parameters);

    /// Handles writes to the macro uploading register.
    void ProcessMacroUpload(u32 data);

    /// Handles writes to the macro bind register.
    void ProcessMacroBind(u32 data);

    /// Handles firmware blob 4
    void ProcessFirmwareCall4();

    /// Handles a write to the CLEAR_BUFFERS register.
    void ProcessClearBuffers();

    /// Handles a write to the QUERY_GET register.
    void ProcessQueryGet();

    // Handles Conditional Rendering
    void ProcessQueryCondition();

    /// Handles writes to syncing register.
    void ProcessSyncPoint();

    /// Handles a write to the CB_DATA[i] register.
    void StartCBData(u32 method);
    void ProcessCBData(u32 value);
    void FinishCBData();

    /// Handles a write to the CB_BIND register.
    void ProcessCBBind(std::size_t stage_index);

    /// Handles a write to the VERTEX_END_GL register, triggering a draw.
    void DrawArrays();

    // Handles a instance drawcall from MME
    void StepInstance(MMEDrawMode expected_mode, u32 count);
};

#define ASSERT_REG_POSITION(field_name, position)                                                  \
    static_assert(offsetof(Maxwell3D::Regs, field_name) == position * 4,                           \
                  "Field " #field_name " has invalid position")

ASSERT_REG_POSITION(macros, 0x45);
ASSERT_REG_POSITION(upload, 0x60);
ASSERT_REG_POSITION(exec_upload, 0x6C);
ASSERT_REG_POSITION(data_upload, 0x6D);
ASSERT_REG_POSITION(sync_info, 0xB2);
ASSERT_REG_POSITION(tess_mode, 0xC8);
ASSERT_REG_POSITION(tess_level_outer, 0xC9);
ASSERT_REG_POSITION(tess_level_inner, 0xCD);
ASSERT_REG_POSITION(rasterize_enable, 0xDF);
ASSERT_REG_POSITION(tfb_enabled, 0x1D1);
ASSERT_REG_POSITION(rt, 0x200);
ASSERT_REG_POSITION(viewport_transform, 0x280);
ASSERT_REG_POSITION(viewports, 0x300);
ASSERT_REG_POSITION(vertex_buffer, 0x35D);
ASSERT_REG_POSITION(depth_mode, 0x35F);
ASSERT_REG_POSITION(clear_color[0], 0x360);
ASSERT_REG_POSITION(clear_depth, 0x364);
ASSERT_REG_POSITION(clear_stencil, 0x368);
ASSERT_REG_POSITION(polygon_offset_point_enable, 0x370);
ASSERT_REG_POSITION(polygon_offset_line_enable, 0x371);
ASSERT_REG_POSITION(polygon_offset_fill_enable, 0x372);
ASSERT_REG_POSITION(patch_vertices, 0x373);
ASSERT_REG_POSITION(scissor_test, 0x380);
ASSERT_REG_POSITION(stencil_back_func_ref, 0x3D5);
ASSERT_REG_POSITION(stencil_back_mask, 0x3D6);
ASSERT_REG_POSITION(stencil_back_func_mask, 0x3D7);
ASSERT_REG_POSITION(color_mask_common, 0x3E4);
ASSERT_REG_POSITION(rt_separate_frag_data, 0x3EB);
ASSERT_REG_POSITION(depth_bounds, 0x3E7);
ASSERT_REG_POSITION(zeta, 0x3F8);
ASSERT_REG_POSITION(clear_flags, 0x43E);
ASSERT_REG_POSITION(vertex_attrib_format, 0x458);
ASSERT_REG_POSITION(rt_control, 0x487);
ASSERT_REG_POSITION(zeta_width, 0x48a);
ASSERT_REG_POSITION(zeta_height, 0x48b);
ASSERT_REG_POSITION(depth_test_enable, 0x4B3);
ASSERT_REG_POSITION(independent_blend_enable, 0x4B9);
ASSERT_REG_POSITION(depth_write_enabled, 0x4BA);
ASSERT_REG_POSITION(alpha_test_enabled, 0x4BB);
ASSERT_REG_POSITION(d3d_cull_mode, 0x4C2);
ASSERT_REG_POSITION(depth_test_func, 0x4C3);
ASSERT_REG_POSITION(alpha_test_ref, 0x4C4);
ASSERT_REG_POSITION(alpha_test_func, 0x4C5);
ASSERT_REG_POSITION(draw_tfb_stride, 0x4C6);
ASSERT_REG_POSITION(blend_color, 0x4C7);
ASSERT_REG_POSITION(blend, 0x4CF);
ASSERT_REG_POSITION(stencil_enable, 0x4E0);
ASSERT_REG_POSITION(stencil_front_op_fail, 0x4E1);
ASSERT_REG_POSITION(stencil_front_op_zfail, 0x4E2);
ASSERT_REG_POSITION(stencil_front_op_zpass, 0x4E3);
ASSERT_REG_POSITION(stencil_front_func_func, 0x4E4);
ASSERT_REG_POSITION(stencil_front_func_ref, 0x4E5);
ASSERT_REG_POSITION(stencil_front_func_mask, 0x4E6);
ASSERT_REG_POSITION(stencil_front_mask, 0x4E7);
ASSERT_REG_POSITION(frag_color_clamp, 0x4EA);
ASSERT_REG_POSITION(screen_y_control, 0x4EB);
ASSERT_REG_POSITION(vb_element_base, 0x50D);
ASSERT_REG_POSITION(vb_base_instance, 0x50E);
ASSERT_REG_POSITION(clip_distance_enabled, 0x544);
ASSERT_REG_POSITION(point_size, 0x546);
ASSERT_REG_POSITION(point_sprite_enable, 0x548);
ASSERT_REG_POSITION(zeta_enable, 0x54E);
ASSERT_REG_POSITION(multisample_control, 0x54F);
ASSERT_REG_POSITION(condition, 0x554);
ASSERT_REG_POSITION(tsc, 0x557);
ASSERT_REG_POSITION(polygon_offset_factor, 0x55b);
ASSERT_REG_POSITION(tic, 0x55D);
ASSERT_REG_POSITION(stencil_two_side_enable, 0x565);
ASSERT_REG_POSITION(stencil_back_op_fail, 0x566);
ASSERT_REG_POSITION(stencil_back_op_zfail, 0x567);
ASSERT_REG_POSITION(stencil_back_op_zpass, 0x568);
ASSERT_REG_POSITION(stencil_back_func_func, 0x569);
ASSERT_REG_POSITION(framebuffer_srgb, 0x56E);
ASSERT_REG_POSITION(polygon_offset_units, 0x56F);
ASSERT_REG_POSITION(point_coord_replace, 0x581);
ASSERT_REG_POSITION(code_address, 0x582);
ASSERT_REG_POSITION(draw, 0x585);
ASSERT_REG_POSITION(primitive_restart, 0x591);
ASSERT_REG_POSITION(index_array, 0x5F2);
ASSERT_REG_POSITION(polygon_offset_clamp, 0x61F);
ASSERT_REG_POSITION(instanced_arrays, 0x620);
ASSERT_REG_POSITION(vp_point_size, 0x644);
ASSERT_REG_POSITION(cull, 0x646);
ASSERT_REG_POSITION(pixel_center_integer, 0x649);
ASSERT_REG_POSITION(viewport_transform_enabled, 0x64B);
ASSERT_REG_POSITION(view_volume_clip_control, 0x64F);
ASSERT_REG_POSITION(depth_bounds_enable, 0x66F);
ASSERT_REG_POSITION(logic_op, 0x671);
ASSERT_REG_POSITION(clear_buffers, 0x674);
ASSERT_REG_POSITION(color_mask, 0x680);
ASSERT_REG_POSITION(query, 0x6C0);
ASSERT_REG_POSITION(vertex_array[0], 0x700);
ASSERT_REG_POSITION(independent_blend, 0x780);
ASSERT_REG_POSITION(vertex_array_limit[0], 0x7C0);
ASSERT_REG_POSITION(shader_config[0], 0x800);
ASSERT_REG_POSITION(firmware, 0x8C0);
ASSERT_REG_POSITION(const_buffer, 0x8E0);
ASSERT_REG_POSITION(cb_bind[0], 0x904);
ASSERT_REG_POSITION(tex_cb_index, 0x982);
ASSERT_REG_POSITION(ssbo_info, 0xD18);
ASSERT_REG_POSITION(tex_info_buffers.address[0], 0xD2A);
ASSERT_REG_POSITION(tex_info_buffers.size[0], 0xD2F);

#undef ASSERT_REG_POSITION

} // namespace Tegra::Engines
