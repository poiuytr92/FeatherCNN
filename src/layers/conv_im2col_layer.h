//Tencent is pleased to support the open source community by making FeatherCNN available.

//Copyright (C) 2018 THL A29 Limited, a Tencent company. All rights reserved.

//Licensed under the BSD 3-Clause License (the "License"); you may not use this file except
//in compliance with the License. You may obtain a copy of the License at
//
//https://opensource.org/licenses/BSD-3-Clause
//
//Unless required by applicable law or agreed to in writing, software distributed
//under the License is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
//CONDITIONS OF ANY KIND, either express or implied. See the License for the
//specific language governing permissions and limitations under the License.

#pragma once

#include "../feather_simple_generated.h"
#include "conv_layer.h"
#include "blob.h"

#include "arm/generic_kernels.h"
#include "arm/sgemm.h"

#include <assert.h>
#include <stdio.h>

#define Enable_NaiveGEMM 0

namespace feather
{
void naive_sgemm(int M, int N, int L, float* A, float* B, float* C)
{
    for (int i = 0; i < M; ++i) //loop over rows in C
    {
        for (int j = 0; j < N; ++j) //loop over columns in C
        {
            float sigma = 0;
            for (int k = 0; k < L; ++k)
            {
                sigma += A[i * L + k] * B[k * N + j];
            }
            C[i * N + j] = sigma;
        }
    }
}
class ConvIm2colLayer : public ConvLayer
{
    public:
        ConvIm2colLayer(const LayerParameter *layer_param, const RuntimeParameter<float>* rt_param)
            : fuse_relu(false), kc(0), nc(0), img_buffer(0), ConvLayer(layer_param, rt_param)
        {
		if(Enable_NaiveGEMM)	_fusible = false;
		else			_fusible = true;
		kc = 304;
		nc = 304;
		//kc = 400;
		//nc = 400;
        }


        int Forward()
        {
            MEMPOOL_CHECK_RETURN(common_mempool->GetPtr(&img_buffer));
	    if(group <=0)	group = 1;
#if (Enable_NaiveGEMM)
/*            if (kernel_width == 1 && kernel_height == 1 && stride_height == 1 && stride_width == 1)
            {
                if (output_channels % 8 == 0)
                {
                    block_sgemm_external_pack_threading_8x8((int)output_channels, (int)output_width * (int)output_height,
                                                            (int)input_channels * (int)kernel_width * (int)kernel_height,
                                                            packed_kernel, input, output, (int)num_threads);
                }
                else
                {
                    block_sgemm_external_pack_threading((int)output_channels, (int)output_width * (int)output_height,
                                                        (int)input_channels * (int)kernel_width * (int)kernel_height,
                                                        packed_kernel, input, output, (int)num_threads);
                }
            }
            else
            {
                Im2col();

                //jintaomeng  support the case for group != input_channels
                int block = (int)input_channels / group * (int)kernel_width * (int)kernel_height;
                if (output_channels % 8 == 0)
                {
                    for (int k = 0; k < group; k++)
                        block_sgemm_external_pack_threading_8x8((int)output_channels, (int)output_width * (int)output_height,
                                                                (int)input_channels / group * (int)kernel_width * (int)kernel_height,
                                                                packed_kernel, img_buffer + k * block, output, (int)num_threads);
                }
                else
                {

                    for (int k = 0; k < group; k++)
                        block_sgemm_external_pack_threading((int)output_channels, (int)output_width * (int)output_height,
                                                            (int)input_channels / group * (int)kernel_width * (int)kernel_height,
                                                            packed_kernel, img_buffer + k * block, output, (int)num_threads);
                }
            }
*/
	    printf("naiveGEMM\n");
            Im2col();
            naive_sgemm(output_channels, output_height * output_width, input_channels * kernel_width * kernel_height, kernel_data, img_buffer, output);

            if (bias_term)
            {
                size_t out_stride = output_width * output_height;
                for (int i = 0; i < output_channels; ++i)
                {
                    float bias = bias_data[i];
                    for (int j = 0; j < out_stride; ++j)
                    {
                        output[out_stride * i + j] = output[out_stride * i + j] + bias;
                    }
                }
            }
#else
	    printf("newGEMM\n");
	    const int M = output_channels;
	    const int N = output_height * output_width;
	    const int K = input_channels * kernel_width * kernel_height;
	    if (kernel_width == 1 && kernel_height == 1 && stride_height == 1 && stride_width == 1)
	    {
		    packed_sgemm(M, N, K, packed_kernel, input, N, output, N, nc, kc, bias_data, num_threads);
	    }
	    else
	    {
		    Im2col();
		    packed_sgemm(M, N, K, packed_kernel, img_buffer, N, output, N, nc, kc, bias_data, num_threads);
	    }
#endif
            return 0;
        }

        int Fuse(Layer *next_layer)
        {
            if (next_layer->type().compare("ReLU") == 0)
            {
                fuse_relu = true;
                return 1;
            }
            else
            {
                return 0;
            }
        }

        bool Im2col()
        {
            const int stride = kernel_height * kernel_width * output_height * output_width;
            if ((kernel_width == 1 && kernel_height == 1) && (stride_height == 2 && stride_width == 2))
            {
                float* ret = img_buffer;
                #pragma omp parallel for num_threads(num_threads)
                for (int k = 0; k < input_channels; k++)
                {
                    int retID = stride * k;
                    {
                        for (int i = 0; i < output_height; i++)
                        {
                            for (int j = 0; j < output_width; j++)
                            {
                                //calculate each row
                                int row = 2 * i - (int)padding_top;
                                int col = 2 * j - (int)padding_left;
                                if (row < 0 || row >= input_height || col < 0 || col >= input_width)
                                {
                                    ret[retID] = 0;
                                }
                                else
                                {
                                    size_t index  =  k * input_width * input_height + row * input_width + col; //(i+u)*input_width+j+v;
                                    ret[retID] = input[index];
                                }
                                retID++;
                            }
                        }
                    }
                }

            }
            else
            {
                float* ret = img_buffer;
                #pragma omp parallel for num_threads(num_threads)
                for (int k = 0; k < input_channels; k++)
                {
                    int retID = stride * k;
                    for (int u = 0; u < kernel_height; u++)   for (int v = 0; v < kernel_width; v++)
                        {
                            for (int i = 0; i < output_height; i++)
                            {
                                for (int j = 0; j < output_width; j++)
                                {
                                    //calculate each row
                                    int row = u - (int)padding_top  + i * (int)stride_height;
                                    int col = v - (int)padding_left + j * (int)stride_width;
                                    //printf("row %d, col %d\n", row, col);
                                    if (row < 0 || row >= input_height || col < 0 || col >= input_width)
                                    {
                                        ret[retID] = 0;
                                    }
                                    else
                                    {
                                        size_t index  =  k * input_width * input_height + row * input_width + col; //(i+u)*input_width+j+v;
                                        ret[retID] = input[index];
                                    }
                                    retID++;
                                }
                            }
                        }
                }
            }
            return true;
        }

        int Init()
        {
            int M = (int)output_channels;
	    int N = output_height * output_width;
            int K = (int)input_channels * (int)kernel_height * (int)kernel_width;
//            int eM = M + (8 - M % 8) % 8;

            MEMPOOL_CHECK_RETURN(common_mempool->Request(sizeof(float) * (input_channels * kernel_height * kernel_width) * (output_width * output_height)));

#if 1
            MEMPOOL_CHECK_RETURN(private_mempool.Alloc(&packed_kernel, sizeof(float) * M * K));
	    packed_sgemm_init<8>(M, K, kc, packed_kernel, kernel_data, K);
	    if(bias_term && fuse_relu)
		    packed_sgemm = packed_sgemm_activation<true, true>;
	    else if(bias_term)
		    packed_sgemm = packed_sgemm_activation<true, false>;
	    else if(fuse_relu)
		    packed_sgemm = packed_sgemm_activation<false, true>;
	    else
		    packed_sgemm = packed_sgemm_activation<false, false>;

#else
            if (M % 8 == 0)
            {
                externalPackA8(M, K, packed_kernel, kernel_data, K);
            }
            else
            {
                externalPackA(M, K, packed_kernel, kernel_data, K);
            }
#endif
            //Setup input and output pointers.
            input = _bottom_blobs[_bottom[0]]->data();
            //_bottom_blobs[_bottom[0]]->PrintBlobInfo();
            output = _top_blobs[_top[0]]->data();
            //_top_blobs[_top[0]]->PrintBlobInfo();
            //printf("++stride %d %d\n", stride_height, stride_width);
            //printf("++padding %d %d %d %d\n", padding_left, padding_top, padding_right, padding_bottom);
            //printf("++kernel %d %d\n", kernel_width, kernel_height);
            //printf("++bias term %d\n", bias_term);
            return 0;
        }

    private:
        float* packed_kernel;
        float* img_buffer;

        float* input;
        float* output;
	bool fuse_relu;
	int  kc, nc;
	void (*packed_sgemm)(int M, int N, int K, float *packA, float *b, int ldb, float *c, int ldc, int nc, int kc, float* bias, int num_threads);
};
};
