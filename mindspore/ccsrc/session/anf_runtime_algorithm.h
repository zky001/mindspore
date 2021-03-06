/**
 * Copyright 2019 Huawei Technologies Co., Ltd
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef MINDSPORE_CCSRC_SESSION_ANF_RUNTIME_ALGORITHM_H
#define MINDSPORE_CCSRC_SESSION_ANF_RUNTIME_ALGORITHM_H
#include <iostream>
#include <string>
#include <vector>
#include <set>
#include <tuple>
#include <utility>
#include <memory>
#include "ir/anf.h"
#include "ir/dtype.h"
#include "ir/base.h"
#include "ir/primitive.h"
#include "device/device_address.h"
#include "kernel/kernel.h"
#include "kernel/kernel_build_info.h"
#include "operator/ops.h"

namespace mindspore {
namespace session {
using AnfVisitFuncion = std::function<Any(const AnfNodePtr &node, int index)>;
using KernelWithIndex = std::pair<AnfNodePtr, size_t>;
class AnfRuntimeAlgorithm {
 public:
  // get input_anf_node's real kernel by recurse
  static KernelWithIndex VisitKernel(const AnfNodePtr &input_anf_node, size_t output_index);
  static KernelWithIndex VisitKernelWithReturnType(const AnfNodePtr &input_anf_node, size_t output_index,
                                                   const std::vector<PrimitivePtr> &return_types = {
                                                     prim::kPrimMakeTuple});
  static std::vector<AnfNodePtr> GetAllOutput(const AnfNodePtr &node,
                                              const std::vector<PrimitivePtr> &return_types = {});
  // get cnode primitive
  static AnfNodePtr GetCNodePrimitiveNode(const CNodePtr &node);
  static void SetNodeInput(const CNodePtr &node, const AnfNodePtr &input_node, size_t index);
  static PrimitivePtr GetCNodePrimitive(const AnfNodePtr &node);
  // check whether anf node is a node of 'primitive_type',such as make_tuple is a cnode of kPrimMakeTuple
  static bool CheckPrimitiveType(const AnfNodePtr &node, const PrimitivePtr &primitive_type);
  // get kernel_name of anf node
  static std::string GetCNodeName(const AnfNodePtr &node);
  // get detail info of anf node
  static std::string GetNodeDebugString(const AnfNodePtr &node);
  // get attr of anf node
  template <typename T>
  static T GetNodeAttr(const AnfNodePtr &node, const std::string &key) {
    MS_EXCEPTION_IF_NULL(node);
    if (!node->isa<CNode>()) {
      std::string node_debug_log = node->DebugString();
      MS_LOG(EXCEPTION) << "Only cnode has attr, but this anf is " << node_debug_log.c_str();
    }
    auto primitive = GetCNodePrimitive(node);
    MS_EXCEPTION_IF_NULL(primitive);
    return GetValue<T>(primitive->GetAttr(key));
  }
  static bool IsTupleOutput(const AnfNodePtr &anf);
  // set attr of anf node
  static void SetNodeAttr(const std::string &key, const ValuePtr &value, const AnfNodePtr &node);
  // set attr of key from 'from' node to 'to' node
  static void CopyNodeAttr(const std::string &key, const AnfNodePtr &from, const AnfNodePtr &to);
  // set a new key for attr from 'from' node to 'to' node
  static void CopyNodeAttr(const std::string &old_key, const std::string &new_key, const AnfNodePtr &from,
                           const AnfNodePtr &to);
  // set all attrs from 'from' node to 'to' node
  static void CopyNodeAttrs(const AnfNodePtr &from, const AnfNodePtr &to);
  // check whether a cnode has the specified attr.
  static bool HasNodeAttr(const std::string &key, const AnfNodePtr &node);
  // delete attr of anf node
  static void EraseNodeAttr(const std::string &key, AnfNodePtr node);
  // get the num of input real_kernel(which can be build and run in device)
  static size_t GetInputTensorNum(const AnfNodePtr &node);
  // get the num of output real_kernel(which can be build and run in device)
  static size_t GetOutputTensorNum(const AnfNodePtr &node);
  // get output format select of anf node
  static std::string GetOutputFormat(const AnfNodePtr &node, size_t output_idx);
  // get input format select of anf node
  static std::string GetInputFormat(const AnfNodePtr &node, size_t input_idx);
  // get output format from prev node,input_index is the input index of current node related to prev node
  static std::string GetPrevNodeOutputFormat(const AnfNodePtr &node, size_t input_idx);
  // get output shapes inferred by ME from input nodes.
  static std::vector<size_t> GetOutputInferShape(const AnfNodePtr &node, size_t output_idx);
  // get input shapes inferred by ME from input nodes.
  static std::vector<size_t> GetPrevNodeOutputInferShape(const AnfNodePtr &node, size_t input_idx);
  // get output shapes which will built and run in device
  static std::vector<size_t> GetOutputDeviceShape(const AnfNodePtr &node, size_t output_idx);
  // get input shapes which will built and run in device
  static std::vector<size_t> GetInputDeviceShape(const AnfNodePtr &node, size_t input_idx);
  static std::vector<kernel::Axis> GetInputReshapeType(const AnfNodePtr &node, size_t output_idx);
  static std::vector<kernel::Axis> GetOutputReshapeType(const AnfNodePtr &node, size_t output_idx);
  // get output data type inferred by ME of anf node
  static TypeId GetOutputInferDataType(const AnfNodePtr &node, size_t output_idx);
  // get output original data type from prev node,input_index is the input index of current node related to prev node
  static TypeId GetPrevNodeOutputInferDataType(const AnfNodePtr &node, size_t input_idx);
  // get output select data type of anf node
  static TypeId GetOutputDeviceDataType(const AnfNodePtr &node, size_t output_idx);
  // get input select data type of anf node
  static TypeId GetInputDeviceDataType(const AnfNodePtr &node, size_t input_idx);
  // get output select data type from prev node,input_index is the input index of current node related to prev node
  static TypeId GetPrevNodeOutputDeviceDataType(const AnfNodePtr &node, size_t input_idx);
  // get output device addr of anf_node
  static const DeviceAddress *GetOutputAddr(const AnfNodePtr &node, size_t output_idx);
  // get mutable output device addr of anf_node
  static DeviceAddressPtr GetMutableOutputAddr(const AnfNodePtr &node, size_t output_idx);
  // check whether output addr is exist or not
  static bool OutputAddrExist(const AnfNodePtr &node, size_t output_idx);
  // get address from prev node,input_index is the input index of current node related to prev node
  static const DeviceAddress *GetPrevNodeOutputAddr(const AnfNodePtr &node, size_t input_idx);
  static DeviceAddressPtr GetPrevNodeMutableOutputAddr(const AnfNodePtr &anf_node, size_t input_idx);
  // set output device addr of anf_node
  static void SetOutputAddr(const DeviceAddressPtr &addr, size_t output_idx, AnfNode *node);
  // set workspace device addr of anf_node
  static void SetWorkspaceAddr(const DeviceAddressPtr &addr, size_t output_idx, AnfNode *node);
  // get workspace device addr of anf_node
  static DeviceAddress *GetWorkspaceAddr(const AnfNodePtr &node, size_t output_idx);
  // set infer shapes and types of anf node
  static void SetOutputInferTypeAndShape(const std::vector<TypeId> &types,
                                         const std::vector<std::vector<size_t>> &shapes, AnfNode *node);
  static void CopyAbstract(const AnfNodePtr &from_node, AnfNode *to_node);
  // get KernelBuildType of node ,such as ATT,RT,FWK and so on
  static KernelType GetKernelType(const AnfNodePtr &node);
  // get processor type:AICORE,AICPU...
  static kernel::Processor GetProcessor(const AnfNodePtr &node);
  // get fusion type:AICORE,AICPU...
  static kernel::FusionType GetFusionType(const AnfNodePtr &node);
  // set select kernel_build_info
  static void SetSelectKernelBuildInfo(const kernel::KernelBuildInfoPtr &select_kernel_build_info, AnfNode *node);
  // get select kernel_build_info
  static kernel::KernelBuildInfoPtr GetSelectKernelBuildInfo(const AnfNodePtr &node);
  // get kernelMode
  static kernel::KernelMod *GetKernelMod(const AnfNodePtr &node);
  // set kernel mod
  static void SetKernelMod(const kernel::KernelModPtr &kernel_mod, AnfNode *node);
  // checkout whether the anf node is a real kernel that can run on device,parameter and constant is real kernel too
  static bool IsRealKernel(const AnfNodePtr &node);
  // checkout whether the anf node is a real kernel that is a cnode and can run on device
  static bool IsRealCNodeKernel(const AnfNodePtr &node);
  // check parameter is weight or data
  static bool IsParameterWeight(const ParameterPtr &node);
  // set stream id of kernel,which will be set in stream assign and be used in stream generate
  static void SetStreamId(uint32_t stream_id, AnfNode *node);
  // get stream id
  static uint32_t GetStreamId(const AnfNodePtr &node);
  // set stream distinction label to distinguish different ops in different streams
  static void SetStreamDistinctionLabel(uint32_t stream_label, AnfNode *node);
  // get stream distinction label
  static uint32_t GetStreamDistinctionLabel(const AnfNode *node);
  // set graph id
  static void SetGraphId(uint32_t graph_id, AnfNode *node);
  // get graph id
  static uint32_t GetGraphId(const AnfNode *node);
  static AnfNodePtr GetInputNode(const CNodePtr &node, size_t index);
  static bool IsFeatureMapInput(const AnfNodePtr &node, size_t input_index);
  // get real input index for some tbe ops which input order is different between me and tbe impl
  static size_t GetRealInputIndex(const AnfNodePtr &anf_node, const size_t cur_index);
  static bool IsCommunicationOp(const AnfNodePtr &node);
};
}  // namespace session
using AnfAlgo = session::AnfRuntimeAlgorithm;
}  // namespace mindspore
#endif  // MINDSPORE_CCSRC_SESSION_ANF_RUNTIME_ALGORITHM_H
