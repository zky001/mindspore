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
#include "session/kernel_graph.h"
#include <algorithm>
#include <queue>
#include <stack>
#include <unordered_set>
#include "common/utils.h"
#include "operator/ops.h"
#include "session/anf_runtime_algorithm.h"
#include "device/kernel_info.h"
#include "kernel/kernel_build_info.h"

namespace mindspore {
namespace session {
namespace {
void PushNoVisitedNode(const AnfNodePtr &node, std::queue<AnfNodePtr> *que,
                       std::unordered_set<AnfNodePtr> *visited_nodes) {
  MS_EXCEPTION_IF_NULL(que);
  MS_EXCEPTION_IF_NULL(visited_nodes);
  if (visited_nodes->find(node) == visited_nodes->end()) {
    que->push(node);
    (void)visited_nodes->insert(node);
    MS_LOG(DEBUG) << "Push que:" << node->DebugString();
  }
}
}  // namespace
std::vector<AnfNodePtr> KernelGraph::outputs() const {
  MS_EXCEPTION_IF_NULL(output());
  if (IsPrimitiveCNode(output(), prim::kPrimMakeTuple)) {
    auto make_tuple = output()->cast<CNodePtr>();
    MS_EXCEPTION_IF_NULL(make_tuple);
    auto &inputs = make_tuple->inputs();
    return std::vector<AnfNodePtr>(inputs.begin() + 1, inputs.end());
  }
  return std::vector<AnfNodePtr>();
}

void KernelGraph::SetExecOrderByDefault() {
  BfsToUpdateNodeOutput();
  execution_order_.clear();
  std::queue<AnfNodePtr> allreduce_nodes;
  std::queue<AnfNodePtr> zero_output_nodes;
  std::unordered_set<AnfNodePtr> visited_nodes;
  auto clear_output = [&zero_output_nodes, &allreduce_nodes, &visited_nodes, this](const AnfNodePtr &input) -> void {
    if (node_output_num_[input] == 0 && visited_nodes.find(input) == visited_nodes.end()) {
      MS_EXCEPTION_IF_NULL(input);
      MS_LOG(DEBUG) << "Clear output num:" << input->DebugString();
      (void)visited_nodes.insert(input);
      if (input->isa<CNode>() && AnfAlgo::GetCNodeName(input) == kAllReduceOpName) {
        allreduce_nodes.push(input);
      } else {
        zero_output_nodes.push(input);
      }
    }
  };
  zero_output_nodes.emplace(get_return());
  while (!zero_output_nodes.empty() || !allreduce_nodes.empty()) {
    AnfNodePtr node;
    if (!zero_output_nodes.empty()) {
      node = zero_output_nodes.front();
      zero_output_nodes.pop();
    } else {
      node = allreduce_nodes.front();
      allreduce_nodes.pop();
    }
    MS_EXCEPTION_IF_NULL(node);
    if (node->isa<CNode>() && AnfAlgo::IsRealKernel(node)) {
      execution_order_.push_back(node->cast<CNodePtr>());
    }
    auto it = node_input_edges_.find(node);
    if (it == node_input_edges_.end()) {
      // value node and parameter has no input,no need to print log
      if (node->isa<CNode>()) {
        MS_LOG(DEBUG) << "Can not find node [" << node->DebugString() << "]";
      }
      continue;
    }
    for (const auto &input_edge : it->second) {
      if (node_output_num_.find(input_edge.first) == node_output_num_.end()) {
        MS_EXCEPTION_IF_NULL(input_edge.first);
        MS_LOG(EXCEPTION) << "Can't find node[" << input_edge.first->DebugString() << "]";
      }
      MS_EXCEPTION_IF_NULL(input_edge.first);
      MS_LOG(DEBUG) << "Decrease input:" << input_edge.first->DebugString() << ",node:" << node->DebugString()
                    << ",num: " << node_output_num_[input_edge.first] << ",decrease num:" << input_edge.second;
      if (node_output_num_[input_edge.first] < input_edge.second) {
        MS_LOG(EXCEPTION) << "Input node:" << input_edge.first->DebugString() << ",node_output_num"
                          << node_output_num_[input_edge.first] << "depend edge:" << input_edge.second;
      }
      node_output_num_[input_edge.first] = node_output_num_[input_edge.first] - input_edge.second;
      clear_output(input_edge.first);
    }
  }
  CheckLoop();
  std::reverse(execution_order_.begin(), execution_order_.end());
}

void KernelGraph::CheckLoop() {
  std::map<AnfNodePtr, size_t> none_zero_output;
  if (node_output_edges_.size() != node_output_num_.size()) {
    MS_LOG(EXCEPTION) << "node_output_edges_ size :" << node_output_edges_.size()
                      << "not equal to node_output_num_ size:" << node_output_num_.size();
  }
  for (auto &it : node_output_num_) {
    MS_EXCEPTION_IF_NULL(it.first);
    string str;
    auto node_output_it = node_output_edges_.find(it.first);
    if (node_output_it == node_output_edges_.end()) {
      MS_LOG(EXCEPTION) << "Can't find node [" << it.first->DebugString() << "]";
    }
    for (const auto &output_edge : node_output_edges_[it.first]) {
      MS_EXCEPTION_IF_NULL(output_edge.first);
      str = str.append(output_edge.first->DebugString()).append("|");
    }
    if (it.second != 0) {
      MS_LOG(WARNING) << "Node:" << it.first->DebugString() << ",outputs:" << str << ",output num:" << it.second;
      none_zero_output[it.first] = it.second;
    }
  }
  // if don't consider control depend and loop exit,a exception will be throw
  if (!none_zero_output.empty()) {
    MS_LOG(EXCEPTION) << "Nodes have loop, left node num:" << none_zero_output.size();
  }
}

CNodePtr KernelGraph::NewCNode(const std::vector<AnfNodePtr> &inputs) {
  auto cnode = FuncGraph::NewCNode(inputs);
  MS_EXCEPTION_IF_NULL(cnode);
  cnode->set_abstract(std::make_shared<abstract::AbstractNone>());
  // create kernel_info from new parameter
  auto kernel_info = std::make_shared<device::KernelInfo>();
  cnode->set_kernel_info(kernel_info);
  AnfAlgo::SetGraphId(graph_id_, cnode.get());
  return cnode;
}

CNodePtr KernelGraph::NewCNode(const CNodePtr &cnode) {
  MS_EXCEPTION_IF_NULL(cnode);
  auto new_cnode = std::make_shared<CNode>(*cnode);
  // if a cnode is created not from front,this cnode won't be in map,so when replace it,we shouldn't update map
  if (BackendNodeExistInFrontBackendMap(cnode)) {
    FrontBackendlMapUpdate(cnode, new_cnode);
  }
  AnfAlgo::SetGraphId(graph_id_, cnode.get());
  return new_cnode;
}

ParameterPtr KernelGraph::NewParameter(const ParameterPtr &parameter) {
  ParameterPtr new_parameter = add_parameter();
  MS_EXCEPTION_IF_NULL(new_parameter);
  size_t output_tensor_num = 1;
  // if use default parameter = nullptr,it remarks create a new parameter from no parameter
  if (parameter == nullptr) {
    new_parameter->set_abstract(std::make_shared<abstract::AbstractNone>());
  } else {
    // if don't use default parameter = nullptr,it remarks create a new parameter from a old parameter
    new_parameter->set_abstract(parameter->abstract());
    new_parameter->set_name(parameter->name());
    if (parameter->has_default()) {
      new_parameter->set_default_param(parameter->default_param());
    }
    // if output is a tuple tensor,now can use for loop to handle tuple tensor
    output_tensor_num = AnfAlgo::GetOutputTensorNum(parameter);
  }
  // create kernel_info form new parameter
  auto kernel_info = std::make_shared<device::KernelInfo>();
  new_parameter->set_kernel_info(kernel_info);
  // create kernel_build_info for new parameter
  auto kernel_build_info_builder = std::make_shared<kernel::KernelBuildInfo::KernelBuildInfoBuilder>();
  // create init data type,
  std::vector<TypeId> init_data_type = {};
  for (size_t i = 0; i < output_tensor_num; i++) {
    TypeId infer_data_type = AnfAlgo::GetOutputInferDataType(new_parameter, i);
    init_data_type.push_back(AnfAlgo::IsParameterWeight(new_parameter) ? kTypeUnknown : infer_data_type);
  }
  // set the format of parameter to DEFAULT_FORMAT
  kernel_build_info_builder->SetOutputsFormat(std::vector<std::string>(output_tensor_num, kOpFormat_DEFAULT));
  // set parameter initaial device data type
  kernel_build_info_builder->SetOutputsDeviceType(init_data_type);
  AnfAlgo::SetSelectKernelBuildInfo(kernel_build_info_builder->Build(), new_parameter.get());
  AnfAlgo::SetGraphId(graph_id_, new_parameter.get());
  return new_parameter;
}

std::vector<AnfNodePtr> KernelGraph::SplitTupleValueNodeToNodeList(const ValueNodePtr &value_node) {
  MS_EXCEPTION_IF_NULL(value_node);
  auto node_value = value_node->value();
  auto output_size = AnfAlgo::GetOutputTensorNum(value_node);
  std::vector<AnfNodePtr> convert_inputs;
  if (!node_value->isa<ValueTuple>()) {
    MS_LOG(EXCEPTION) << "multiple output valuenode's value must be a value tuple but got " << node_value->ToString();
  }
  auto value_tuple = node_value->cast<ValueTuplePtr>();
  MS_EXCEPTION_IF_NULL(value_tuple);
  if (value_tuple->size() != output_size) {
    MS_LOG(EXCEPTION) << "value tuple size" << value_tuple->size()
                      << " is not mathced with the value node's output size" << output_size;
  }
  for (size_t index = 0; index < value_tuple->value().size(); ++index) {
    auto new_value_node = std::make_shared<ValueNode>(value_tuple->value()[index]);
    AnfAlgo::SetOutputInferTypeAndShape({AnfAlgo::GetOutputInferDataType(value_node, index)},
                                        {AnfAlgo::GetOutputInferShape(value_node, index)}, new_value_node.get());
    AddValueNodeToGraph(new_value_node);
    auto kernel_info = std::make_shared<device::KernelInfo>();
    new_value_node->set_kernel_info(kernel_info);
    // create kernel_build_info for new value node
    auto kernel_build_info_builder = std::make_shared<kernel::KernelBuildInfo::KernelBuildInfoBuilder>();
    // set the format of value_node to DEFAULT_FORMAT
    kernel_build_info_builder->SetOutputsFormat({kOpFormat_DEFAULT});
    // set value node initial device data type = infer data type
    kernel_build_info_builder->SetOutputsDeviceType({kTypeUnknown});
    AnfAlgo::SetSelectKernelBuildInfo(kernel_build_info_builder->Build(), new_value_node.get());
    AnfAlgo::SetGraphId(graph_id_, new_value_node.get());
    AddValueNodeToGraph(new_value_node);
    convert_inputs.emplace_back(new_value_node);
  }
  if (RemoveValueNodeFromGraph(value_node)) {
    MS_LOG(WARNING) << "failed to remove the value_node " << value_node->DebugString();
  }
  return convert_inputs;
}

ValueNodePtr KernelGraph::NewValueNode(const ValueNodePtr &value_node) {
  MS_EXCEPTION_IF_NULL(value_node);
  ValueNodePtr new_value_node = std::make_shared<ValueNode>(value_node->value());
  new_value_node->set_abstract(value_node->abstract());
  // create kernel_info fo new value node
  auto kernel_info = std::make_shared<device::KernelInfo>();
  new_value_node->set_kernel_info(kernel_info);
  // create kernel_build_info for new value node
  auto kernel_build_info_builder = std::make_shared<kernel::KernelBuildInfo::KernelBuildInfoBuilder>();
  // set the format of value_node to DEFAULT_FORMAT
  kernel_build_info_builder->SetOutputsFormat(std::vector<std::string>{kOpFormat_DEFAULT});
  // set value node initial device data type = infer data type
  std::vector<TypeId> types;
  for (size_t index = 0; index < AnfAlgo::GetOutputTensorNum(value_node); ++index) {
    types.push_back(kTypeUnknown);
  }
  kernel_build_info_builder->SetOutputsDeviceType(types);
  AnfAlgo::SetSelectKernelBuildInfo(kernel_build_info_builder->Build(), new_value_node.get());
  AnfAlgo::SetGraphId(graph_id_, new_value_node.get());
  return new_value_node;
}

const std::vector<AnfNodePtr> &KernelGraph::inputs() const {
  MS_EXCEPTION_IF_NULL(inputs_);
  return *inputs_;
}

void KernelGraph::FrontBackendlMapAdd(const AnfNodePtr &front_anf, const AnfNodePtr &backend_anf) {
  MS_EXCEPTION_IF_NULL(front_anf);
  MS_EXCEPTION_IF_NULL(backend_anf);
  if (front_backend_anf_map_.find(front_anf) != front_backend_anf_map_.end()) {
    MS_LOG(EXCEPTION) << "anf " << front_anf->DebugString() << " has been exist in the front_backend_anf_map_";
  }
  if (backend_front_anf_map_.find(backend_anf) != backend_front_anf_map_.end()) {
    MS_LOG(EXCEPTION) << "kernel " << backend_anf->DebugString() << "has been exist in the backend_front_anf_map_";
  }
  front_backend_anf_map_[front_anf] = backend_anf;
  backend_front_anf_map_[backend_anf] = front_anf;
}

void KernelGraph::FrontBackendlMapUpdate(const AnfNodePtr &old_backend_anf, const AnfNodePtr &new_backend_anf) {
  MS_EXCEPTION_IF_NULL(old_backend_anf);
  MS_EXCEPTION_IF_NULL(new_backend_anf);
  if (old_backend_anf.get() == new_backend_anf.get()) {
    MS_LOG(EXCEPTION) << "old can't be same with new";
  }
  if (backend_front_anf_map_.find(old_backend_anf) == backend_front_anf_map_.end()) {
    MS_LOG(EXCEPTION) << "old_backend_anf " << old_backend_anf->DebugString() << " is not exist in the map";
  }
  if (front_backend_anf_map_.find(backend_front_anf_map_[old_backend_anf]) == front_backend_anf_map_.end()) {
    MS_LOG(EXCEPTION) << "anf is not exist in the mape ,old " << old_backend_anf->DebugString();
  }
  front_backend_anf_map_[backend_front_anf_map_[old_backend_anf]] = new_backend_anf;
  backend_front_anf_map_[new_backend_anf] = backend_front_anf_map_[old_backend_anf];
  // delete old kernel
  (void)backend_front_anf_map_.erase(old_backend_anf);
}
// get kernel by anf
AnfNodePtr KernelGraph::GetBackendAnfByFrontAnf(const AnfNodePtr &front_anf) {
  if (front_backend_anf_map_.find(front_anf) == front_backend_anf_map_.end()) {
    return nullptr;
  }
  return front_backend_anf_map_[front_anf];
}

bool KernelGraph::BackendNodeExistInFrontBackendMap(const AnfNodePtr &backend_anf) {
  return backend_front_anf_map_.find(backend_anf) != backend_front_anf_map_.end();
}

ValueNodePtr KernelGraph::GetValueNodeByTensor(const mindspore::tensor::TensorPtr &tensor) {
  if (tensor_to_value_node_map_.find(tensor) == tensor_to_value_node_map_.end()) {
    return nullptr;
  }
  return tensor_to_value_node_map_[tensor];
}

void KernelGraph::TensorValueNodeMapAdd(const tensor::TensorPtr &tensor, const ValueNodePtr &value_node) {
  MS_EXCEPTION_IF_NULL(tensor);
  MS_EXCEPTION_IF_NULL(value_node);
  tensor_to_value_node_map_[tensor] = value_node;
}

void KernelGraph::AddDependEdge(const AnfNodePtr &node, const AnfNodePtr &input, size_t depend_edge_num) {
  MS_LOG(DEBUG) << "Input:" << input->DebugString() << ",  node:" << node->DebugString() << ",num:" << depend_edge_num;
  auto output_depend_edge = std::pair<AnfNodePtr, size_t>(node, depend_edge_num);
  // add output depend edge of input
  auto output_it = node_output_edges_.find(input);
  if (output_it == node_output_edges_.end()) {
    node_output_edges_[input] = std::vector<std::pair<AnfNodePtr, size_t>>{output_depend_edge};
  } else {
    output_it->second.push_back(output_depend_edge);
  }
  // add input depend edge of output
  auto input_depend_edge = std::pair<AnfNodePtr, size_t>(input, depend_edge_num);
  auto input_it = node_input_edges_.find(node);
  if (input_it == node_input_edges_.end()) {
    node_input_edges_[node] = std::vector<std::pair<AnfNodePtr, size_t>>{input_depend_edge};
  } else {
    input_it->second.push_back(input_depend_edge);
  }
  // add the depend sum of node
  auto depend_it = node_output_num_.find(input);
  if (depend_it == node_output_num_.end()) {
    node_output_num_[input] = 0;
  }
  node_output_num_[input] += depend_edge_num;
}

std::vector<AnfNodePtr> KernelGraph::GetOutputNodes(const AnfNodePtr &node) {
  MS_EXCEPTION_IF_NULL(node);
  auto it = node_output_edges_.find(node);
  if (it == node_output_edges_.end()) {
    MS_LOG(EXCEPTION) << "Can't find node[" << node->DebugString() << "]";
  }
  std::vector<AnfNodePtr> output_nodes;
  auto trans = [](const std::pair<AnfNodePtr, size_t> &pair) -> AnfNodePtr { return pair.first; };
  (void)std::transform(it->second.begin(), it->second.end(), std::back_inserter(output_nodes), trans);
  return output_nodes;
}

// update the depend relations of control depend
void KernelGraph::UpdateControlDependRelations(const std::vector<AnfNodePtr> &depends) {
  for (const auto &node : depends) {
    MS_EXCEPTION_IF_NULL(node);
    if (!node->isa<CNode>()) {
      return;
    }
    auto cnode = node->cast<CNodePtr>();
    MS_EXCEPTION_IF_NULL(cnode);
    if (!AnfAlgo::CheckPrimitiveType(node, prim::kPrimControlDepend)) {
      MS_LOG(EXCEPTION) << node->DebugString() << " is not a control depend";
    }
    auto prior_node = cnode->input(kControlDependPriorIndex);
    auto depend_node = cnode->input(kControlDependBehindIndex);
    MS_EXCEPTION_IF_NULL(prior_node);
    MS_EXCEPTION_IF_NULL(depend_node);
    std::vector<AnfNodePtr> prior_nodes = {prior_node};
    std::vector<AnfNodePtr> depend_nodes = {depend_node};
    MS_LOG(INFO) << "Prior node[" << prior_node->DebugString() << "],depend node[" << depend_node->DebugString()
                 << "],depend_mode=[" << AnfAlgo::GetNodeAttr<int>(cnode, "depend_mode") << "]";
    if (prior_node->isa<Parameter>()) {
      prior_nodes = GetOutputNodes(prior_node);
    }
    if (depend_node->isa<Parameter>()) {
      depend_nodes = GetOutputNodes(depend_node);
    }
    for (auto &first_node : prior_nodes) {
      for (auto &second_node : depend_nodes) {
        MS_EXCEPTION_IF_NULL(first_node);
        MS_EXCEPTION_IF_NULL(second_node);
        MS_LOG(INFO) << "Add first node:" << first_node->DebugString() << ",second node:" << second_node->DebugString();
        AddDependEdge(second_node, first_node, 1);
      }
    }
  }
}

bool KernelGraph::HandleControlDependNode(const AnfNodePtr &node, std::queue<AnfNodePtr> *que,
                                          std::unordered_set<AnfNodePtr> *visited_nodes) {
  MS_EXCEPTION_IF_NULL(node);
  if (!node->isa<CNode>()) {
    return false;
  }
  auto cnode = node->cast<CNodePtr>();
  MS_EXCEPTION_IF_NULL(cnode);
  if (!AnfAlgo::CheckPrimitiveType(node, prim::kPrimControlDepend)) {
    return false;
  }
  // set the control depend visited but don't push it into the que
  if (visited_nodes->find(node) != visited_nodes->end()) {
    MS_LOG(EXCEPTION) << "control depend[" << node->DebugString() << "] has been handled before";
  }
  (void)visited_nodes->insert(cnode);
  // add a 0 depend num to keep the link relations to prepare for finding zero output nodes
  auto prior_node = cnode->input(kControlDependPriorIndex);
  auto depend_node = cnode->input(kControlDependBehindIndex);
  for (const auto &input : cnode->inputs()) {
    AddDependEdge(node, input, 0);
  }
  PushNoVisitedNode(depend_node, que, visited_nodes);
  PushNoVisitedNode(prior_node, que, visited_nodes);
  return true;
}

void KernelGraph::BfsToUpdateNodeOutput() {
  node_output_edges_.clear();
  node_output_num_.clear();
  node_input_edges_.clear();
  std::vector<AnfNodePtr> control_depends;
  std::unordered_set<AnfNodePtr> visited_nodes;
  std::queue<AnfNodePtr> que;
  que.push(get_return());
  while (!que.empty()) {
    auto node = que.front();
    que.pop();
    MS_EXCEPTION_IF_NULL(node);
    if (!node->isa<CNode>()) {
      continue;
    }
    auto cnode = node->cast<CNodePtr>();
    MS_EXCEPTION_IF_NULL(cnode);
    // handle data links
    for (const auto &input : cnode->inputs()) {
      size_t depend_edge_num = 1;
      // handle control depend,all inputs of control depend has no depend edge
      if (HandleControlDependNode(input, &que, &visited_nodes)) {
        control_depends.push_back(input);
        depend_edge_num = 0;
      }
      // the 2rd input of depend is no depend edge
      if (AnfAlgo::CheckPrimitiveType(node, prim::kPrimDepend) && input == cnode->input(kDependAttachNodeIndex)) {
        depend_edge_num = 0;
      }
      PushNoVisitedNode(input, &que, &visited_nodes);
      AddDependEdge(node, input, depend_edge_num);
    }
  }
  UpdateControlDependRelations(control_depends);
}

void KernelGraph::AddValueNodeToGraph(const ValueNodePtr &value_node) { (void)graph_value_nodes_.insert(value_node); }

bool KernelGraph::IsInRefOutputMap(const AnfWithOutIndex &pair) const { return ref_out_in_map_.count(pair) != 0; }

AnfWithOutIndex KernelGraph::GetRefCorrespondOutput(const AnfWithOutIndex &out_pair) const {
  if (!IsInRefOutputMap(out_pair)) {
    MS_LOG(EXCEPTION) << "out_pair is not in RefOutputMap";
  }
  return ref_out_in_map_.at(out_pair);
}

void KernelGraph::AddRefCorrespondPairs(const AnfWithOutIndex &final_pair, const AnfWithOutIndex &origin_pair) {
  if (IsInRefOutputMap(final_pair)) {
    MS_LOG(EXCEPTION) << "out_pair is already in RefOutputMap";
  }
  (void)ref_out_in_map_.insert(std::make_pair(final_pair, origin_pair));
}

bool KernelGraph::RemoveValueNodeFromGraph(const ValueNodePtr &value_node) {
  if (graph_value_nodes_.find(value_node) != graph_value_nodes_.end()) {
    (void)graph_value_nodes_.erase(value_node);
    return true;
  }
  return false;
}
}  // namespace session
}  // namespace mindspore
