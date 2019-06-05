/*
 * Copyright @2017 Intel Corporation
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library. If not, see <http://www.gnu.org/licenses/>.
 *
 */

#include <math.h>
#include <cutils/properties.h>
#include "gpu_executor.h"
#include "vk_common.h"
#include "vk_cs_executor.h"
#include "shader/spv_shader.h"

NAME_SPACE_BEGIN

// TODO: query group count from vulkan device
#define MAX_GROUP_COUNT_X 65535
#define MAX_GROUP_COUNT_Y 65535
#define MAX_GROUP_COUNT_Z 65535

struct PushConst {
public:
    PushConst() {};
};

struct SpecializaitonConst {
public:
    SpecializaitonConst() {};
    SpecializaitonConst(int ih, int iw, int oh, int ow, int fh,
                        int fw, int chn, int bat, int M, int K, int N):
        in_h(ih), in_w(iw), out_h(oh), out_w(ow), filter_h(fh), filter_w(fw),
        channels(chn), batch(bat), m(M), k(K), n(N)
    {};

    int local_sz_x;
    int local_sz_y;
    int local_sz_z;
    int in_h;
    int in_w;
    int out_h;
    int out_w;
    int stride_h;
    int stride_w;
    int pad_h;
    int pad_w;
    int filter_h;
    int filter_w;
    int channels;
    int batch;
    int m;
    int k;
    int n;
    int activation;
};

void computeConvOutputShapeAndPadding(const PaddingScheme& padding_mode,
                                      const uint32_t& in_h, const uint32_t& in_w,
                                      const uint32_t& filter_h, const uint32_t& filter_w,
                                      const uint32_t& dilation_h, const uint32_t& dilation_w,
                                      const uint32_t& stride_h, const uint32_t& stride_w,
                                      uint32_t& out_h, uint32_t& out_w)
{
    if (padding_mode == kPaddingValid)
    {
        out_h = ceil((in_h - (filter_h - 1) * dilation_h) / stride_h);
        out_w = ceil((in_w - (filter_w - 1) * dilation_w) / stride_w);
    }
    else if (padding_mode == kPaddingSame)
    {
        out_h = ceil(in_h / stride_h);
        out_w = ceil(in_w / stride_w);
    }
    else
    {
        LOGE("Invalid padding mode:%d", padding_mode);
    }
}

static void prepareConfig(const Operation& operation, ShaderConfig& config)
{
    //tune();
    (void)(operation);
    (void)(config);
}

bool VkCsExecutor::convolve(const Operation& operation, ShaderConfig& config)
{
#define BUFFER_NUM 4
    opBase->initVulkanThing(BUFFER_NUM);

    const hidl_vec<uint32_t>& ins  = operation.inputs;
    const hidl_vec<uint32_t>& outs = operation.outputs;

    const size_t inCount = ins.size();
    ASSERT(inCount == 10 || inCount == 7);

    VkOperand& in     = operands[ins[0]];
    VkOperand& filter = operands[ins[1]];
    VkOperand& bias   = operands[ins[2]];
    VkOperand& out    = operands[outs[0]];

    Shape in_shape     = in.getShape();
    Shape out_shape    = out.getShape();
    Shape filter_shape = filter.getShape();
    Shape bias_shape   = bias.getShape();

    int M = out_shape[kShapeIdxHeight] * out_shape[kShapeIdxWidth];
    int N = out_shape[kShapeIdxChannel];
    int K = in_shape[kShapeIdxChannel] * filter_shape[kShapeIdxHeight] * filter_shape[kShapeIdxWidth];

    PaddingScheme padding_mode;

    SpecializaitonConst spec_const(in_shape[kShapeIdxHeight], in_shape[kShapeIdxWidth],
                                   out_shape[kShapeIdxHeight], out_shape[kShapeIdxWidth],
                                   filter_shape[kShapeIdxHeight], filter_shape[kShapeIdxWidth],
                                   in_shape[kShapeIdxChannel], in_shape[kShapeIdxBatch], M, K, N);

    // todo: need a wrapper, just for 3*3*1 && 3*3*2 input here.
    spec_const.local_sz_x = 1;
    spec_const.local_sz_y = 16;
    if (in_shape[kShapeIdxChannel] == 1)
    {
        spec_const.local_sz_z = 1;
    }
    else
    {
        spec_const.local_sz_z = 4;
    }

    PushConst push_const;

    if (opBase->pipeline == VK_NULL_HANDLE)
    {
        if (inCount == 10)
        {
            uint32_t padding_left   = operands[ins[3]].getScalarData<uint32_t>();
            uint32_t padding_right  = operands[ins[4]].getScalarData<uint32_t>();
            uint32_t padding_top    = operands[ins[5]].getScalarData<uint32_t>();
            uint32_t padding_bottom = operands[ins[6]].getScalarData<uint32_t>();

            spec_const.pad_w        = padding_left;
            spec_const.pad_h        = padding_top;
            spec_const.stride_w     = operands[ins[7]].getScalarData<uint32_t>();
            spec_const.stride_h     = operands[ins[8]].getScalarData<uint32_t>();
            spec_const.activation   = operands[ins[9]].getScalarData<uint32_t>();

            // assert(spec_const.activation == 0);  todo: add activation

            if (padding_left == 0 && padding_top == 0)
            {
                padding_mode = kPaddingValid;
            }
            else
            {
                padding_mode = kPaddingSame;
            }
        }
        else
        {
            padding_mode          = static_cast<PaddingScheme>(operands[ins[3]].getScalarData<uint32_t>());
            spec_const.stride_w   = operands[ins[4]].getScalarData<uint32_t>();
            spec_const.stride_h   = operands[ins[5]].getScalarData<uint32_t>();
            spec_const.activation = operands[ins[6]].getScalarData<uint32_t>();

            assert(spec_const.activation == 0);

            calculateExplicitPadding(spec_const.in_w, spec_const.stride_w, spec_const.filter_w,
                                     padding_mode, &spec_const.pad_w);
            calculateExplicitPadding(spec_const.in_h, spec_const.stride_h, spec_const.filter_h,
                                     padding_mode, &spec_const.pad_h);
        }

#define SPEC_CONST_NUM 19
        VkSpecializationMapEntry entry[SPEC_CONST_NUM];

        SET_SPEC_CONST_ENTRY(entry[0], 0, offsetof(SpecializaitonConst, local_sz_x), sizeof(int));
        SET_SPEC_CONST_ENTRY(entry[1], 1, offsetof(SpecializaitonConst, local_sz_y), sizeof(int));
        SET_SPEC_CONST_ENTRY(entry[2], 2, offsetof(SpecializaitonConst, local_sz_z), sizeof(int));
        SET_SPEC_CONST_ENTRY(entry[3], 3, offsetof(SpecializaitonConst, in_h), sizeof(int));
        SET_SPEC_CONST_ENTRY(entry[4], 4, offsetof(SpecializaitonConst, in_w), sizeof(int));
        SET_SPEC_CONST_ENTRY(entry[5], 5, offsetof(SpecializaitonConst, out_h), sizeof(int));
        SET_SPEC_CONST_ENTRY(entry[6], 6, offsetof(SpecializaitonConst, out_w), sizeof(int));
        SET_SPEC_CONST_ENTRY(entry[7], 7, offsetof(SpecializaitonConst, stride_h), sizeof(int));
        SET_SPEC_CONST_ENTRY(entry[8], 8, offsetof(SpecializaitonConst, stride_w), sizeof(int));
        SET_SPEC_CONST_ENTRY(entry[9], 9, offsetof(SpecializaitonConst, pad_h), sizeof(int));
        SET_SPEC_CONST_ENTRY(entry[10], 10, offsetof(SpecializaitonConst, pad_w), sizeof(int));
        SET_SPEC_CONST_ENTRY(entry[11], 11, offsetof(SpecializaitonConst, filter_h), sizeof(int));
        SET_SPEC_CONST_ENTRY(entry[12], 12, offsetof(SpecializaitonConst, filter_w), sizeof(int));
        SET_SPEC_CONST_ENTRY(entry[13], 13, offsetof(SpecializaitonConst, channels), sizeof(int));
        SET_SPEC_CONST_ENTRY(entry[14], 14, offsetof(SpecializaitonConst, batch), sizeof(int));
        SET_SPEC_CONST_ENTRY(entry[15], 15, offsetof(SpecializaitonConst, m), sizeof(int));
        SET_SPEC_CONST_ENTRY(entry[16], 16, offsetof(SpecializaitonConst, k), sizeof(int));
        SET_SPEC_CONST_ENTRY(entry[17], 17, offsetof(SpecializaitonConst, n), sizeof(int));
        SET_SPEC_CONST_ENTRY(entry[18], 18, offsetof(SpecializaitonConst, activation), sizeof(int));

        VkSpecializationInfo spec_info;

        spec_info.mapEntryCount = SPEC_CONST_NUM;
        spec_info.pMapEntries   = entry;
        spec_info.dataSize      = sizeof(spec_const);
        spec_info.pData         = &spec_const;

        NN_GPU_DEBUG("run createShaderModule");
        opBase->createShaderModule(conv_spv, sizeof(conv_spv));

        NN_GPU_DEBUG("run createPipeline");
        opBase->createPipeline(sizeof(PushConst), &spec_info);
    }


    opBase->group_x = alignSize(alignSize(N, config.block_width) / config.block_width, spec_const.local_sz_x) / spec_const.local_sz_x;
    opBase->group_y = alignSize(alignSize(M, config.block_height) / config.block_height, spec_const.local_sz_y) / spec_const.local_sz_y;
    opBase->group_z = alignSize(alignSize(spec_const.batch, config.block_depth), spec_const.local_sz_z) / spec_const.local_sz_z;

    NN_GPU_DEBUG("bind operands");
    opBase->bindOperand(in, 0, opBase->descriptor_set);
    opBase->bindOperand(filter, 1, opBase->descriptor_set);
    opBase->bindOperand(bias, 2, opBase->descriptor_set);
    opBase->bindOperand(out, 3, opBase->descriptor_set);

    int partition_num = (int)ceil(1.0 * N / opBase->group_y);

    for (uint32_t b = 0; b < in_shape[kShapeIdxBatch]; b++)
    {
        for (int n = 0; n < partition_num; n++)
        {
            NN_GPU_DEBUG("do recordCommandBuffer");
            opBase->recordCommandBuffer((void*)&push_const, sizeof(PushConst));

            NN_GPU_DEBUG("do runCommandBuffer");
            opBase->runCommandBuffer();
        }
    }

    return true;
}

// FIXME:
// Android NN don't set group, dilation, has_bias,
// so make these assumptions: group = 1, dilation = 1, has_bias = 1
bool VkCsExecutor::doCONV_2D(const Operation& operation)
{
    ASSERT(operation.type == OperationType::CONV_2D);

    ShaderConfig config = {1, 16, 1, 1, 1, 1};
    prepareConfig(operation, config);
    return convolve(operation, config);
}

NAME_SPACE_STOP
