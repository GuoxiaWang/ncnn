// Tencent is pleased to support the open source community by making ncnn available.
//
// Copyright (C) 2019 THL A29 Limited, a Tencent company. All rights reserved.
//
// Licensed under the BSD 3-Clause License (the "License"); you may not use this file except
// in compliance with the License. You may obtain a copy of the License at
//
// https://opensource.org/licenses/BSD-3-Clause
//
// Unless required by applicable law or agreed to in writing, software distributed
// under the License is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
// CONDITIONS OF ANY KIND, either express or implied. See the License for the
// specific language governing permissions and limitations under the License.

#include "deconvolution_vulkan.h"
#include <algorithm>
#include "layer_type.h"

namespace ncnn {

DEFINE_LAYER_CREATOR(Deconvolution_vulkan)

Deconvolution_vulkan::Deconvolution_vulkan()
{
    support_vulkan = true;

    crop = 0;
    output_pad = 0;
    output_crop = 0;

    pipeline_deconvolution = 0;
    pipeline_deconvolution_pack4 = 0;
    pipeline_deconvolution_pack1to4 = 0;
    pipeline_deconvolution_pack4to1 = 0;
}

int Deconvolution_vulkan::create_pipeline(const Option& opt)
{
    {
        crop = ncnn::create_layer(ncnn::LayerType::Crop);
        crop->vkdev = vkdev;

        ncnn::ParamDict pd;
        pd.set(0, pad_left);
        pd.set(1, pad_top);
        pd.set(2, 0);

        crop->load_param(pd);

        crop->create_pipeline(opt);
    }

    {
        output_pad = ncnn::create_layer(ncnn::LayerType::Padding);
        output_pad->vkdev = vkdev;

        ncnn::ParamDict pd;
        pd.set(0, 0);
        pd.set(1, output_pad_bottom);
        pd.set(2, 0);
        pd.set(3, output_pad_right);
        pd.set(4, 0);
        pd.set(5, 0.f);

        output_pad->load_param(pd);

        output_pad->create_pipeline(opt);
    }

    {
        output_crop = ncnn::create_layer(ncnn::LayerType::Crop);
        output_crop->vkdev = vkdev;

        ncnn::ParamDict pd;
        pd.set(0, -233);
        pd.set(1, -233);
        pd.set(2, -233);

        output_crop->load_param(pd);

        output_crop->create_pipeline(opt);
    }

    const int maxk = kernel_w * kernel_h;
    int num_input = weight_data_size / maxk / num_output;

    std::vector<vk_specialization_type> specializations(10);
    specializations[0].i = kernel_w;
    specializations[1].i = kernel_h;
    specializations[2].i = dilation_w;
    specializations[3].i = dilation_h;
    specializations[4].i = stride_w;
    specializations[5].i = stride_h;
    specializations[6].i = bias_term;
    specializations[7].i = activation_type;
    specializations[8].f = activation_params.w == 1 ? activation_params[0] : 0.f;
    specializations[9].f = activation_params.w == 2 ? activation_params[1] : 0.f;

    // pack1
    if (num_input % 4 != 0 && num_output % 4 != 0)
    {
        pipeline_deconvolution = new Pipeline(vkdev);
        pipeline_deconvolution->set_optimal_local_size_xyz(32, 32, std::max(1, num_output / 8));
        pipeline_deconvolution->create("deconvolution", opt, specializations, 4, 10);
    }

    // pack4
    if (num_input % 4 == 0 && num_output % 4 == 0)
    {
        pipeline_deconvolution_pack4 = new Pipeline(vkdev);
        pipeline_deconvolution_pack4->set_optimal_local_size_xyz(32, 32, std::max(1, num_output / 8));
        pipeline_deconvolution_pack4->create("deconvolution_pack4", opt, specializations, 4, 10);
    }

    // pack1to4
    if (num_input % 4 != 0 && num_output % 4 == 0)
    {
        pipeline_deconvolution_pack1to4 = new Pipeline(vkdev);
        pipeline_deconvolution_pack1to4->set_optimal_local_size_xyz(32, 32, std::max(1, num_output / 8));
        pipeline_deconvolution_pack1to4->create("deconvolution_pack1to4", opt, specializations, 4, 10);
    }

    // pack4to1
    if (num_input % 4 == 0 && num_output % 4 != 0)
    {
        pipeline_deconvolution_pack4to1 = new Pipeline(vkdev);
        pipeline_deconvolution_pack4to1->set_optimal_local_size_xyz(32, 32, std::max(1, num_output / 8));
        pipeline_deconvolution_pack4to1->create("deconvolution_pack4to1", opt, specializations, 4, 10);
    }

    return 0;
}

int Deconvolution_vulkan::destroy_pipeline(const Option& opt)
{
    if (crop)
    {
        crop->destroy_pipeline(opt);
        delete crop;
        crop = 0;
    }

    if (output_pad)
    {
        output_pad->destroy_pipeline(opt);
        delete output_pad;
        output_pad = 0;
    }

    if (output_crop)
    {
        output_crop->destroy_pipeline(opt);
        delete output_crop;
        output_crop = 0;
    }

    delete pipeline_deconvolution;
    pipeline_deconvolution = 0;

    delete pipeline_deconvolution_pack4;
    pipeline_deconvolution_pack4 = 0;

    delete pipeline_deconvolution_pack1to4;
    pipeline_deconvolution_pack1to4 = 0;

    delete pipeline_deconvolution_pack4to1;
    pipeline_deconvolution_pack4to1 = 0;

    return 0;
}

int Deconvolution_vulkan::upload_model(VkTransfer& cmd, const Option& opt)
{
    const int maxk = kernel_w * kernel_h;
    int num_input = weight_data_size / maxk / num_output;

    Mat weight_data_transposed(weight_data.w);
    {
        float* pt = weight_data_transposed;
        const float* p = weight_data;

        for (int i=0; i<num_input*num_output; i++)
        {
            for (int k=0; k<maxk; k++)
            {
                pt[maxk-1 - k] = p[k];
            }

            p += maxk;
            pt += maxk;
        }
    }

    // pack1
    if (num_input % 4 != 0 && num_output % 4 != 0)
    {
        cmd.record_upload(weight_data_transposed, weight_data_gpu, opt);
    }

    // pack4
    if (num_input % 4 == 0 && num_output % 4 == 0)
    {
        // src = kw-kh-inch-outch
        // dst = 4a-4b-kw-kh-inch/4a-outch/4b
        Mat weight_data_pack4;
        {
            Mat weight_data_r2 = weight_data_transposed.reshape(maxk, num_input, num_output);

            weight_data_pack4.create(maxk, num_input/4, num_output/4, (size_t)4*16, 16);

            for (int q=0; q+3<num_output; q+=4)
            {
                const Mat k0 = weight_data_r2.channel(q);
                const Mat k1 = weight_data_r2.channel(q+1);
                const Mat k2 = weight_data_r2.channel(q+2);
                const Mat k3 = weight_data_r2.channel(q+3);

                Mat g0 = weight_data_pack4.channel(q/4);

                for (int p=0; p+3<num_input; p+=4)
                {
                    const float* k00 = k0.row(p);
                    const float* k01 = k0.row(p+1);
                    const float* k02 = k0.row(p+2);
                    const float* k03 = k0.row(p+3);

                    const float* k10 = k1.row(p);
                    const float* k11 = k1.row(p+1);
                    const float* k12 = k1.row(p+2);
                    const float* k13 = k1.row(p+3);

                    const float* k20 = k2.row(p);
                    const float* k21 = k2.row(p+1);
                    const float* k22 = k2.row(p+2);
                    const float* k23 = k2.row(p+3);

                    const float* k30 = k3.row(p);
                    const float* k31 = k3.row(p+1);
                    const float* k32 = k3.row(p+2);
                    const float* k33 = k3.row(p+3);

                    float* g00 = g0.row(p/4);

                    for (int k=0; k<maxk; k++)
                    {
                        g00[0] = k00[k];
                        g00[1] = k01[k];
                        g00[2] = k02[k];
                        g00[3] = k03[k];

                        g00[4] = k10[k];
                        g00[5] = k11[k];
                        g00[6] = k12[k];
                        g00[7] = k13[k];

                        g00[8] = k20[k];
                        g00[9] = k21[k];
                        g00[10] = k22[k];
                        g00[11] = k23[k];

                        g00[12] = k30[k];
                        g00[13] = k31[k];
                        g00[14] = k32[k];
                        g00[15] = k33[k];

                        g00 += 16;
                    }
                }
            }
        }

        cmd.record_upload(weight_data_pack4, weight_data_gpu_pack4, opt);
    }

    // pack1to4
    if (num_input % 4 != 0 && num_output % 4 == 0)
    {
        // src = kw-kh-inch-outch
        // dst = 4b-kw-kh-inch-outch/4b
        Mat weight_data_pack1to4;
        {
            Mat weight_data_r2 = weight_data_transposed.reshape(maxk, num_input, num_output);

            weight_data_pack1to4.create(maxk, num_input, num_output/4, (size_t)4*4, 4);

            for (int q=0; q+3<num_output; q+=4)
            {
                const Mat k0 = weight_data_r2.channel(q);
                const Mat k1 = weight_data_r2.channel(q+1);
                const Mat k2 = weight_data_r2.channel(q+2);
                const Mat k3 = weight_data_r2.channel(q+3);

                Mat g0 = weight_data_pack1to4.channel(q/4);

                for (int p=0; p<num_input; p++)
                {
                    const float* k00 = k0.row(p);
                    const float* k10 = k1.row(p);
                    const float* k20 = k2.row(p);
                    const float* k30 = k3.row(p);

                    float* g00 = g0.row(p);

                    for (int k=0; k<maxk; k++)
                    {
                        g00[0] = k00[k];
                        g00[1] = k10[k];
                        g00[2] = k20[k];
                        g00[3] = k30[k];

                        g00 += 4;
                    }
                }
            }
        }

        cmd.record_upload(weight_data_pack1to4, weight_data_gpu_pack1to4, opt);
    }

    // pack4to1
    if (num_input % 4 == 0 && num_output % 4 != 0)
    {
        // src = kw-kh-inch-outch
        // dst = 4a-kw-kh-inch/4a-outch
        Mat weight_data_pack4to1;
        {
            Mat weight_data_r2 = weight_data_transposed.reshape(maxk, num_input, num_output);

            weight_data_pack4to1.create(maxk, num_input/4, num_output, (size_t)4*4, 4);

            for (int q=0; q<num_output; q++)
            {
                const Mat k0 = weight_data_r2.channel(q);
                Mat g0 = weight_data_pack4to1.channel(q);

                for (int p=0; p+3<num_input; p+=4)
                {
                    const float* k00 = k0.row(p);
                    const float* k01 = k0.row(p+1);
                    const float* k02 = k0.row(p+2);
                    const float* k03 = k0.row(p+3);

                    float* g00 = g0.row(p/4);

                    for (int k=0; k<maxk; k++)
                    {
                        g00[0] = k00[k];
                        g00[1] = k01[k];
                        g00[2] = k02[k];
                        g00[3] = k03[k];

                        g00 += 4;
                    }
                }
            }
        }

        cmd.record_upload(weight_data_pack4to1, weight_data_gpu_pack4to1, opt);
    }

    if (bias_term)
    {
        if (num_output % 4 != 0)
        {
            cmd.record_upload(bias_data, bias_data_gpu, opt);
        }

        if (num_output % 4 == 0)
        {
            Mat bias_data_pack4;
            convert_packing(bias_data, bias_data_pack4, 4);
            cmd.record_upload(bias_data_pack4, bias_data_gpu_pack4, opt);
        }
    }

    return 0;
}

int Deconvolution_vulkan::forward(const VkMat& bottom_blob, VkMat& top_blob, VkCompute& cmd, const Option& opt) const
{
    int w = bottom_blob.w;
    int h = bottom_blob.h;
    size_t elemsize = bottom_blob.elemsize;
    int elempack = bottom_blob.elempack;

    const int kernel_extent_w = dilation_w * (kernel_w - 1) + 1;
    const int kernel_extent_h = dilation_h * (kernel_h - 1) + 1;

    int outw = (w - 1) * stride_w + kernel_extent_w;
    int outh = (h - 1) * stride_h + kernel_extent_h;
    int out_elempack = num_output % 4 == 0 ? 4 : 1;
    size_t out_elemsize = elemsize / elempack * out_elempack;

    if (opt.use_fp16_packed && !opt.use_fp16_storage)
    {
        if (out_elempack == 4) out_elemsize = 4*2u;
        if (out_elempack == 1) out_elemsize = 4u;
    }

    VkMat top_blob_bordered;
    if (pad_left > 0 || pad_right > 0 || pad_top > 0 || pad_bottom > 0 || output_pad_right > 0 || output_pad_bottom > 0 || (output_w > 0 && output_h > 0))
    {
        top_blob_bordered.create(outw, outh, num_output / out_elempack, out_elemsize, out_elempack, opt.workspace_vkallocator, opt.staging_vkallocator);
    }
    else
    {
        top_blob_bordered.create(outw, outh, num_output / out_elempack, out_elemsize, out_elempack, opt.blob_vkallocator, opt.staging_vkallocator);
    }
    if (top_blob_bordered.empty())
        return -100;

    std::vector<VkMat> bindings(4);
    bindings[0] = bottom_blob;
    bindings[1] = top_blob_bordered;
    if (elempack == 1 && out_elempack == 1)
    {
        bindings[2] = weight_data_gpu;
        bindings[3] = bias_term ? bias_data_gpu : bindings[2];// TODO use dummy buffer
    }
    else if (elempack == 4 && out_elempack == 4)
    {
        bindings[2] = weight_data_gpu_pack4;
        bindings[3] = bias_term ? bias_data_gpu_pack4 : bindings[2];// TODO use dummy buffer
    }
    else if (elempack == 1 && out_elempack == 4)
    {
        bindings[2] = weight_data_gpu_pack1to4;
        bindings[3] = bias_term ? bias_data_gpu_pack4 : bindings[2];// TODO use dummy buffer
    }
    else if (elempack == 4 && out_elempack == 1)
    {
        bindings[2] = weight_data_gpu_pack4to1;
        bindings[3] = bias_term ? bias_data_gpu : bindings[2];// TODO use dummy buffer
    }

    std::vector<vk_constant_type> constants(10);
    constants[0].i = bottom_blob.dims;
    constants[1].i = bottom_blob.w;
    constants[2].i = bottom_blob.h;
    constants[3].i = bottom_blob.c;
    constants[4].i = bottom_blob.cstep;
    constants[5].i = top_blob_bordered.dims;
    constants[6].i = top_blob_bordered.w;
    constants[7].i = top_blob_bordered.h;
    constants[8].i = top_blob_bordered.c;
    constants[9].i = top_blob_bordered.cstep;

    const Pipeline* pipeline = 0;
    if (elempack == 1 && out_elempack == 1)
    {
        pipeline = pipeline_deconvolution;
    }
    else if (elempack == 4 && out_elempack == 4)
    {
        pipeline = pipeline_deconvolution_pack4;
    }
    else if (elempack == 1 && out_elempack == 4)
    {
        pipeline = pipeline_deconvolution_pack1to4;
    }
    else if (elempack == 4 && out_elempack == 1)
    {
        pipeline = pipeline_deconvolution_pack4to1;
    }

    cmd.record_pipeline(pipeline, bindings, constants, top_blob_bordered);

    if (pad_left > 0 || pad_right > 0 || pad_top > 0 || pad_bottom > 0)
    {
        VkMat top_blob_bordered_adj = top_blob_bordered;
        if (output_pad_right > 0 || output_pad_bottom > 0)
        {
            ncnn::Option opt_pad = opt;
            opt_pad.blob_vkallocator = opt.workspace_vkallocator;
            output_pad->forward(top_blob_bordered, top_blob_bordered_adj, cmd, opt_pad);
            if (top_blob_bordered_adj.empty())
                return -100;
        }

        {
            VkMat reference_blob;
            reference_blob.dims = 2;
            reference_blob.w = top_blob_bordered_adj.w - pad_left - pad_right;
            reference_blob.h = top_blob_bordered_adj.h - pad_top - pad_bottom;
            reference_blob.elempack = 1;

            std::vector<VkMat> crop_bottom_blobs(2);
            crop_bottom_blobs[0] = top_blob_bordered_adj;
            crop_bottom_blobs[1] = reference_blob;
            std::vector<VkMat> crop_top_blobs(1);
            crop->forward(crop_bottom_blobs, crop_top_blobs, cmd, opt);
            top_blob = crop_top_blobs[0];
        }
        if (top_blob.empty())
            return -100;

        outw = top_blob.w;
        outh = top_blob.h;
    }
    else if (output_w > 0 && output_h > 0)
    {
        VkMat top_blob_bordered_adj = top_blob_bordered;
        if (output_pad_right > 0 || output_pad_bottom > 0)
        {
            ncnn::Option opt_pad = opt;
            opt_pad.blob_vkallocator = opt.workspace_vkallocator;
            output_pad->forward(top_blob_bordered, top_blob_bordered_adj, cmd, opt_pad);
            if (top_blob_bordered_adj.empty())
                return -100;
        }

        int wcut = top_blob_bordered_adj.w - output_w;
        int hcut = top_blob_bordered_adj.h - output_h;

        VkMat crop_param_blob(4, (size_t)4u, 1, opt.staging_vkallocator, opt.staging_vkallocator);
        crop_param_blob.prepare_staging_buffer();
        int* crop_params = crop_param_blob.mapped();

        if (pad_left == -233 || pad_right == -233 || pad_top == -233 || pad_bottom == -233)
        {
            // onnx padding=SAME_UPPER
            crop_params[0] = wcut / 2;
            crop_params[1] = hcut / 2;
            crop_params[2] = 0;
            crop_params[3] = top_blob_bordered_adj.w - wcut;
            crop_params[4] = top_blob_bordered_adj.h - hcut;
            crop_params[5] = top_blob_bordered_adj.c;
        }
        else if (pad_left == -234 || pad_right == -234 || pad_top == -234 || pad_bottom == -234)
        {
            // onnx padding=SAME_LOWER
            crop_params[0] = wcut - wcut / 2;
            crop_params[1] = hcut - hcut / 2;
            crop_params[2] = 0;
            crop_params[3] = top_blob_bordered_adj.w - wcut;
            crop_params[4] = top_blob_bordered_adj.h - hcut;
            crop_params[5] = top_blob_bordered_adj.c;
        }

        std::vector<VkMat> crop_inputs(2);
        crop_inputs[0] = top_blob_bordered_adj;
        crop_inputs[1] = crop_param_blob;

        std::vector<VkMat> crop_outputs(1);
        output_crop->forward(crop_inputs, crop_outputs, cmd, opt);
        top_blob = crop_outputs[0];
        if (top_blob.empty())
            return -100;

        outw = top_blob.w;
        outh = top_blob.h;
    }
    else
    {
        if (output_pad_right > 0 || output_pad_bottom > 0)
        {
            output_pad->forward(top_blob_bordered, top_blob, cmd, opt);
            if (top_blob.empty())
                return -100;
        }
        else
        {
            top_blob = top_blob_bordered;
        }
    }

    return 0;
}

} // namespace ncnn
