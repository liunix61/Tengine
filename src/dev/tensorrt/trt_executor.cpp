/*
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * License); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * AS IS BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 * KIND, either express or implied.  See the License for the
 * specific language governing permissions and limitations
 * under the License.
 */

/*
 * Copyright (c) 2021, Open AI Lab
 * Author: lswang@openailab.com
 */

#include "trt_executor.hpp"
#include "trt_helper.hpp"

extern "C"
{
#include "tengine_op.h"
#include "convolution_param.h"
}

#include <cuda_runtime_api.h>

#include <NvInferRuntime.h>

#define DEFAULT_DEVICE_ID 0
#define DEFAULT_MAX_BATCH 128

static Logger gLogger(nvinfer1::ILogger::Severity::kERROR);


int TensorRTEngine::get_type(int mode, nvinfer1::DataType& type)
{
    switch (mode)
    {
        case TENGINE_DT_FP32:
            type = nvinfer1::DataType::kFLOAT;
            break;
        case TENGINE_DT_FP16:
            type = nvinfer1::DataType::kHALF;
            break;
        case TENGINE_DT_INT8:
            type = nvinfer1::DataType::kINT8;
            break;
        case TENGINE_DT_UINT8:
            return -1;
        case TENGINE_DT_INT32:
            type = nvinfer1::DataType::kINT32;
            break;
        default:
            return -1;
    }

    return 0;
}


bool TensorRTEngine::check_if_input_in_map(uint16_t& id, std::map<uint16_t, uint16_t>& map)
{
    auto iter = map.find(id);
    if (map.end() == iter)
    {
        fprintf(stderr, "Tengine: Query tensor(id: %d) failed.\n", id);
        return false;
    }
    return true;
}




TensorRTEngine::TensorRTEngine()
{
    this->tensor_swap_count = 0;
    this->precision = nvinfer1::DataType::kFLOAT;
}


void TensorRTEngine::SetRange(struct ir_tensor* ir_tensor, nvinfer1::ITensor* trt_tensor)
{
    if (nvinfer1::DataType::kINT8 == this->precision)
    {
        if (1 == ir_tensor->quant_param_num)
        {
            float min_value = ir_tensor->scale * -127.0f;
            float max_value = ir_tensor->scale * +127.0f;
            trt_tensor->setDynamicRange(min_value, max_value);
        }
    }
}


void TensorRTEngine::SetRange(struct ir_graph* ir_graph, uint16_t id, nvinfer1::ITensor* trt_tensor)
{
    struct ir_tensor* ir_tensor = get_ir_graph_tensor(ir_graph, id);
    if (nullptr != ir_tensor)
    {
        this->SetRange(ir_tensor, trt_tensor);
    }
}


int TensorRTEngine::Build(struct subgraph* subgraph)
{
    struct ir_graph* ir_graph = subgraph->graph;

    for (uint16_t i = 0; i < subgraph->node_num; i++)
    {
        uint16_t node_id = subgraph->node_list[i];
        auto ir_node = get_ir_graph_node(ir_graph, node_id);

        for (uint8_t j = 0; j < ir_node->input_num; j++)
        {
            struct ir_tensor* ir_tensor = get_ir_graph_tensor(ir_graph, ir_node->input_tensors[j]);
            if (TENSOR_TYPE_INPUT == ir_tensor->tensor_type || TENSOR_TYPE_VAR == ir_tensor->tensor_type)
            {
                if(!AddTensor(ir_graph, ir_tensor))
                {
                    fprintf(stderr, "Cannot add input tensor(id: %d, name: %s) from node(id: %d, name: %s).\n", ir_tensor->idx, ir_tensor->name, ir_node->idx, ir_node->name);
                    return -5;
                }
            }
        }
    }

    for (uint16_t i = 0; i < subgraph->node_num; i++)
    {
        uint16_t node_id = subgraph->node_list[i];
        auto ir_node = get_ir_graph_node(ir_graph, node_id);
        auto op_type = ir_node->op.op_type;

        switch (op_type)
        {
            case OP_BATCHNORM:
                if (!AddBatchNormNode(ir_graph, ir_node))
                {
                    fprintf(stderr, "Tengine: Cannot add BatchNorm op(%d).\n", ir_node->idx);
                    return -6;
                }
                break;
            case OP_CONST:
                continue;
            case OP_CONCAT:
                if (!AddConcatNode(ir_graph, ir_node))
                {
                    fprintf(stderr, "Tengine: Cannot add Concat op(%d).\n", ir_node->idx);
                    return -6;
                }
                break;
            case OP_CONV: {
                if (!AddConvolutionNode(ir_graph, ir_node))
                {
                    fprintf(stderr, "Tengine: Cannot add Convolution op(%d).\n", ir_node->idx);
                    return -6;
                }
                break;
            }
            case OP_CROP: {
                if (!AddCropNode(ir_graph, ir_node))
                {
                    fprintf(stderr, "Tengine: Cannot add Crop op(%d).\n", ir_node->idx);
                    return -6;
                }
                break;
            }
            case OP_DROPOUT:
                if (!AddDropoutNode(ir_graph, ir_node))
                {
                    fprintf(stderr, "Tengine: Cannot add Dropout op(%d).\n", ir_node->idx);
                    return -6;
                }
                break;
            case OP_ELTWISE:
                if (!AddEltwiseLayer(ir_graph, ir_node))
                {
                    fprintf(stderr, "Tengine: Cannot add Eltwise op(%d).\n", ir_node->idx);
                    return -6;
                }
                break;
            case OP_FLATTEN: {
                if (!AddFlattenNode(ir_graph, ir_node))
                {
                    fprintf(stderr, "Tengine: Cannot add Flatten op(%d).\n", ir_node->idx);
                    return -6;
                }
                break;
            }
            case OP_FC: {
                if (!AddFullyConnectedNode(ir_graph, ir_node))
                {
                    fprintf(stderr, "Tengine: Cannot add FullyConnected op(%d).\n", ir_node->idx);
                    return -6;
                }
                break;
            }
            case OP_INTERP: {
                if (!AddInterpNode(ir_graph, ir_node))
                {
                    fprintf(stderr, "Tengine: Cannot add FullyConnected op(%d).\n", ir_node->idx);
                    return -6;
                }
                break;
            }
            case OP_PERMUTE: {
                if (!AddPermuteNode(ir_graph, ir_node))
                {
                    fprintf(stderr, "Tengine: Cannot add Permute op(%d).\n", ir_node->idx);
                    return -6;
                }
                break;
            }
            case OP_POOL: {
                if (!AddPoolingNode(ir_graph, ir_node))
                {
                    fprintf(stderr, "Tengine: Cannot add Pooling op(%d).\n", ir_node->idx);
                    return -6;
                }
                break;
            }
            case OP_RELU:
            case OP_RELU1:
            case OP_RELU6: {
                if (!addReLUNode(ir_graph, ir_node))
                {
                    fprintf(stderr, "Tengine: Cannot add ReLU op(%d).\n", ir_node->idx);
                    return -6;
                }
                break;
            }
            case OP_RESHAPE: {
                if (!AddReshapeNode(ir_graph, ir_node))
                {
                    fprintf(stderr, "Tengine: Cannot add Reshape op(%d).\n", ir_node->idx);
                    return -6;
                }
                break;
            }
            case OP_SLICE:
            {
                if (!AddSliceNode(ir_graph, ir_node))
                {
                    fprintf(stderr, "Tengine: Cannot add Slice op(%d).\n", ir_node->idx);
                    return -6;
                }
            }
            case OP_INPUT:
                continue;
            case OP_SOFTMAX:
            {
                if(!AddSoftmaxNode(ir_graph, ir_node))
                {
                    fprintf(stderr, "Tengine: Cannot add Softmax op(%d).\n", ir_node->idx);
                    return -6;
                }
                break;
            }
            case OP_UPSAMPLE:
            {
                if(!AddUpsampleNode(ir_graph, ir_node))
                {
                    fprintf(stderr, "Tengine: Cannot add Upsample op(%d).\n", ir_node->idx);
                    return -6;
                }
                break;
            }
            default:
                fprintf(stderr, "Tengine: OP(id:%d, op:%d) not be implemented for now.\n", ir_node->idx, ir_node->op.op_type);
                return -6;
        }
    }

    /* set output */
    for(uint8_t i = 0; i < subgraph->output_num; i++)
    {
        struct ir_tensor* output_tensor = get_ir_graph_tensor(ir_graph, subgraph->output_tensor_list[i]);
        uint16_t output_node_id = output_tensor->producer;

        nvinfer1::ILayer* layer = layer_map[output_node_id];

        layer->setPrecision(nvinfer1::DataType::kFLOAT);
        for (int j = 0; j < layer->getNbOutputs(); j++)
        {
            layer->setOutputType(j, nvinfer1::DataType::kFLOAT);
        }

        //layer->getOutput(i)->setName(output_tensor->name);
        auto trt_tensor = this->tensor_real_map[this->tensor_swap_map[output_tensor->idx]];
        trt_tensor->setName(output_tensor->name);
        this->network->markOutput(*trt_tensor);

    }
}


bool TensorRTEngine::AddTensor(struct ir_graph* ir_graph, struct ir_tensor *ir_tensor)
{
    if (0 < this->tensor_swap_map.count(ir_tensor->idx))
        return true;

    int dims[MAX_SHAPE_DIM_NUM];
    int dim = get_tensor_shape(ir_tensor, dims, MAX_SHAPE_DIM_NUM);

    nvinfer1::ITensor* trt_tensor;

    switch (dim)
    {
        case 3:
        {
            nvinfer1::Dims3 dim3(dims[0],dims[1],dims[2]);
            trt_tensor = this->network->addInput(ir_tensor->name, nvinfer1::DataType::kFLOAT, dim3);
            break;
        }
        case 4:
        {
            nvinfer1::Dims4 dim4(dims[0], dims[1], dims[2], dims[3]);
            trt_tensor = this->network->addInput(ir_tensor->name, nvinfer1::DataType::kFLOAT, dim4);
            break;
        }
        default:
        {
            fprintf(stderr, "Tengine: Tensor data type(%d) cannot supported.\n", ir_tensor->data_type);
            return false;
        }
    }

    if(nullptr == trt_tensor)
    {
        fprintf(stderr, "Tengine: Insert tensor(id: %d, name: %s) failed.\n", ir_tensor->idx, ir_tensor->name);
        return false;
    }

    if (nvinfer1::DataType::kINT8 == this->precision)
    {
        if (1 == ir_tensor->quant_param_num)
        {
            float tensor_min_val = ir_tensor->scale * -127.0f;
            float tensor_max_val = ir_tensor->scale * +127.0f;
            trt_tensor->setDynamicRange(tensor_min_val, tensor_max_val);
        }
        else
        {
            fprintf(stderr, "Tengine: Try add INT8 tensor(id: %d, name: %s) failed, quant param was missing.\n", ir_tensor->idx, ir_tensor->name);
        }
    }

    // set tensor name
    struct ir_node* ir_node = get_ir_graph_node(ir_graph, ir_tensor->producer);
    if (OP_CONV == ir_node->op.op_type)
    {
        conv_param* param = (conv_param*)ir_node->op.param_mem;
        if (0 <= param->activation)
        {
            std::string tensor_name = std::string(ir_tensor->name) + std::to_string(ir_tensor->idx);
            trt_tensor->setName(tensor_name.c_str());
        }
        else
        {
            trt_tensor->setName(ir_tensor->name);
        }
    }
    else
    {
        trt_tensor->setName(ir_tensor->name);
    }

    this->tensor_real_map[ir_tensor->idx] = trt_tensor;
    this->tensor_swap_map[ir_tensor->idx] = ir_tensor->idx;

    return true;
}


int TensorRTEngine::PreRun(struct subgraph* subgraph, int gpu_affinity, int mode)
{
    struct ir_graph* ir_graph = subgraph->graph;

    // set tensor_swap_count
    this->tensor_swap_count = subgraph->graph->tensor_num + 1;

    const auto cuda_status = cudaSetDevice(DEFAULT_DEVICE_ID);;
    if (cuda_status != cudaSuccess)
    {
        fprintf(stderr, "Tengine: Cannot lock to socket %d.\n", DEFAULT_DEVICE_ID);
        return -2;
    }

    // TODO: high level api should serialize model and cache serialized model
    this->builder = nvinfer1::createInferBuilder(gLogger.get_logger());
    if (nullptr == this->builder)
    {
        fprintf(stderr, "Tengine: Cannot create nvinfer1::IBuilder object..\n");
        return -3;
    }

    // TODO: adapt to TRT6
    const auto _explicit_batch = 1U << static_cast<uint32_t>(nvinfer1::NetworkDefinitionCreationFlag::kEXPLICIT_BATCH);
    this->network = this->builder->createNetworkV2(_explicit_batch);
    if (nullptr == this->network)
    {
        fprintf(stderr, "Tengine: Cannot create nvinfer1::INetworkDefinition object.\n");
        return -4;
    }

    this->config = this->builder->createBuilderConfig();
    if (!config)
    {
        fprintf(stderr, "Tengine: Cannot create nvinfer1::createBuilderConfig object.\n");
        return -4;
    }

    nvinfer1::DataType type{ nvinfer1::DataType::kFLOAT };
    if (0 != get_type(mode, type))
    {
        fprintf(stderr, "Tengine: Target precision(%d) is not supported.\n", mode);
        return -1;
    }

    switch (type)
    {
        case nvinfer1::DataType::kFLOAT:
        {
#if NV_TENSORRT_MAJOR >= 7
            // check Ampere arch
            if (this->builder->platformHasTf32())
            {
                this->config->setFlag(nvinfer1::BuilderFlag::kTF32);
                this->precision = nvinfer1::DataType::kFLOAT;
            }
            else
            {
                fprintf(stderr, "Tengine: Try using inference precision TF32 failed, rollback.\n");
            }
#endif
            break;
        }
        case nvinfer1::DataType::kHALF:
        {
            // check Ampere arch
            if (this->builder->platformHasFastFp16())
            {
                this->config->setFlag(nvinfer1::BuilderFlag::kFP16);
                this->precision = nvinfer1::DataType::kHALF;
            }
            else
            {
                fprintf(stderr, "Tengine: Target inference precision(%d) is not supported, rollback.\n", mode);
            }
            break;
        }
        case nvinfer1::DataType::kINT8:
        {
            if (this->builder->platformHasFastInt8())
            {
                struct ir_tensor* input = get_ir_graph_tensor(ir_graph, subgraph->input_tensor_list[0]);
                if (nullptr != input && 1 <= input->quant_param_num)
                {
                    this->config->setFlag(nvinfer1::BuilderFlag::kINT8);
                    this->config->setInt8Calibrator(nullptr);
                    this->precision = nvinfer1::DataType::kINT8;
                }
                else
                {
                    fprintf(stderr, "Tengine: Try enable INT8, but network does not have quant params, rollback to FP32.\n");
                }
            }
            else
            {
                fprintf(stderr, "Tengine: Target inference precision(%d) is not supported, rollback.\n", mode);
            }
            break;
        }
        default:
            break;
    }

    this->config->setFlag(nvinfer1::BuilderFlag::kGPU_FALLBACK);
    this->config->setMaxWorkspaceSize(1_GiB);

    /*auto ret = this->builder->getNbDLACores();
    if (this->builder->getNbDLACores() > 0)
    {
        this->config->setDLACore(0);
        this->config->setDefaultDeviceType(nvinfer1::DeviceType::kDLA);
        this->config->setFlag(nvinfer1::BuilderFlag::kGPU_FALLBACK);
    }*/

    // setting layer precision
    if (nvinfer1::DataType::kINT8 == this->precision)
    {
        for (int i = 0; i < this->network->getNbLayers(); ++i)
        {
            auto layer = this->network->getLayer(i);

            // Don't set the precision on non-computation layers as they don't support int8.
            if (layer->getType() != nvinfer1::LayerType::kCONSTANT && layer->getType() != nvinfer1::LayerType::kCONCATENATION && layer->getType() != nvinfer1::LayerType::kSHAPE)
            {
                // set computation precision of the layer
                layer->setPrecision(nvinfer1::DataType::kINT8);
            }

            for (int j = 0; j < layer->getNbOutputs(); ++j)
            {
                std::string tensorName = layer->getOutput(j)->getName();

                // set output type of execution tensors and not shape tensors.
                if (layer->getOutput(j)->isExecutionTensor())
                {
                    layer->setOutputType(j, nvinfer1::DataType::kINT8);
                }
            }
        }
    }

    // Build trt engine
    this->Build(subgraph);

    this->engine = this->builder->buildEngineWithConfig(*this->network, *this->config);
    if(nullptr == this->engine)
    {
        fprintf(stderr, "Tengine: Can not Build engine, please check config.\n");
        return false;
    }

    // allocate gpu mem for input and output
    this->io_tensors.resize(this->engine->getNbBindings());

    for (uint8_t i = 0; i < subgraph->input_num; i++)
    {
        struct ir_tensor* input_tensor = get_ir_graph_tensor(ir_graph, subgraph->input_tensor_list[i]);

        int binding_id = this->engine->getBindingIndex(input_tensor->name);
        if (0 > binding_id)
        {
            fprintf(stderr, "Tengine: Cannot get binding id(%d) for input(%d).\n", binding_id, input_tensor->idx);
            return false;
        }


        int total_size = (int)(input_tensor->elem_num * input_tensor->elem_size);
        void* gpu_mem = nullptr;

        if(cudaSuccess != cudaMalloc(&gpu_mem, total_size))
        {
            fprintf(stderr, "Tengine: Cannot malloc memory for input(%d).\n", input_tensor->idx);
            return false;
        }

        this->io_tensors[binding_id] = gpu_mem;
    }

    for (uint8_t i = 0; i < subgraph->output_num; i++)
    {
        struct ir_tensor* output_tensor = get_ir_graph_tensor(ir_graph, subgraph->output_tensor_list[i]);

        int binding_id = this->engine->getBindingIndex(output_tensor->name);
        if (0 > binding_id)
        {
            fprintf(stderr, "Tengine: Cannot get binding id(%d) for output(%d).\n", binding_id, output_tensor->idx);
            return false;
        }

        int total_size = (int)(output_tensor->elem_num * output_tensor->elem_size);

        if (nullptr == output_tensor->data)
            output_tensor->data = sys_malloc(total_size);

        void* gpu_mem = nullptr;

        if(cudaSuccess != cudaMalloc(&gpu_mem, total_size))
        {
            fprintf(stderr, "Cannot malloc memory for output(%d).\n", output_tensor->idx);
            return false;
        }

        this->io_tensors[binding_id] = gpu_mem;
    }

    this->context = engine->createExecutionContext();
    if(context == nullptr)
    {
        fprintf(stderr, "Cannot create execution context.\n");
        return false;
    }

    return true;
}


int TensorRTEngine::Run(struct subgraph *subgraph)
{
    struct ir_graph* ir_graph = subgraph->graph;

    for (uint8_t i = 0; i < subgraph->input_num; i++)
    {
        struct ir_tensor* input_tensor = get_ir_graph_tensor(ir_graph, subgraph->input_tensor_list[i]);

        int binding_id = this->engine->getBindingIndex(input_tensor->name);
        if (0 > binding_id)
        {
            fprintf(stderr, "Tengine: Cannot get binding id(%d) for input(%d).\n", binding_id, input_tensor->idx);
            return false;
        }

        int total_size = (int)(input_tensor->elem_num * input_tensor->elem_size);
        void* gpu_mem = this->io_tensors[binding_id];

        cudaMemcpy(gpu_mem, input_tensor->data, total_size, cudaMemcpyHostToDevice);
    }

    int batch_size = get_ir_graph_tensor(subgraph->graph, subgraph->input_tensor_list[0])->dims[0];
    const auto ret = this->context->execute(batch_size, this->io_tensors.data());
    if (!ret)
    {
        fprintf(stderr, "Tengine: TensorRT execute error(%d).", ret);
        return -1;
    }

    for (uint8_t i = 0; i < subgraph->output_num; i++)
    {
        struct ir_tensor* output_tensor = get_ir_graph_tensor(ir_graph, subgraph->output_tensor_list[i]);

        int binding_id = this->engine->getBindingIndex(output_tensor->name);
        if (0 > binding_id)
        {
            fprintf(stderr, "Tengine: Cannot get binding id(%d) for output(%d).\n", binding_id, output_tensor->idx);
            return false;
        }

        int total_size = (int)(output_tensor->elem_num * output_tensor->elem_size);
        void* gpu_mem = this->io_tensors[binding_id];

        cudaMemcpy(output_tensor->data, gpu_mem, total_size, cudaMemcpyDeviceToHost);
    }

    return 0;
}

int TensorRTEngine::PoseRun(struct subgraph *subgraph)
{
    for (auto& ptr : this->host_buffer)
        sys_free(ptr);

    this->context->destroy();
    this->engine->destroy();
    this->config->destroy();
    this->network->destroy();
    this->builder->destroy();
}
