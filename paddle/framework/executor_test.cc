/* Copyright (c) 2016 PaddlePaddle Authors. All Rights Reserve.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License. */

#include "paddle/framework/executor.h"

#include <memory>
#include <vector>

#include "gtest/gtest.h"
#include "paddle/framework/attribute.h"
#include "paddle/framework/backward.h"
#include "paddle/framework/block_desc.h"
#include "paddle/framework/op_desc.h"
#include "paddle/framework/op_registry.h"
#include "paddle/framework/operator.h"

USE_OP(elementwise_add);
USE_OP(gaussian_random);
USE_OP(feed);
USE_OP(fetch);
USE_OP(mul);
USE_OP(sum);
USE_OP(squared_l2_distance);

using std::string;
using namespace paddle::platform;
using namespace paddle::framework;

void AddOp(const std::string& type, const VariableNameMap& inputs,
           const VariableNameMap& outputs, AttributeMap attrs,
           paddle::framework::BlockDescBind* block) {
  // insert output
  for (auto kv : outputs) {
    for (auto v : kv.second) {
      auto var = block->NewVar(v);
      var->SetDataType(paddle::framework::DataType::FP32);
    }
  }

  // insert op
  auto op = block->AppendOp();
  op->SetType(type);
  for (auto& kv : inputs) {
    op->SetInput(kv.first, kv.second);
  }
  for (auto& kv : outputs) {
    op->SetOutput(kv.first, kv.second);
  }
  op->SetAttrMap(attrs);
}

std::once_flag set_variable_flag;

// Tensors in feed value variable will only be in CPUPlace
// So we can  memcpy the data from vector<T> to feed_value
template <typename T>
void SetFeedVariable(const std::vector<std::vector<T>>& inputs) {
  typedef std::vector<paddle::framework::Tensor> FeedInputs;
  Variable* g_feed_value = GetGlobalScope()->FindVar("feed_value");
  FeedInputs& feed_inputs = *(g_feed_value->GetMutable<FeedInputs>());
  size_t size = inputs.size();
  feed_inputs.resize(size);
  for (size_t i = 0; i < size; i++) {
    T* dst = feed_inputs[i].mutable_data<T>(
        make_ddim({static_cast<int64_t>(inputs[i].size())}), CPUPlace());
    memcpy(dst, inputs[i].data(), inputs[i].size() * sizeof(T));
  }
}

// Tensors in fetch value variable will only be in CPUPlace
// So we can memcpy the data from fetch_value to vector<T>
template <typename T>
std::vector<std::vector<T>> GetFetchVariable() {
  typedef std::vector<paddle::framework::Tensor> FetchOutputs;
  Variable* g_fetch_value = GetGlobalScope()->FindVar("fetch_value");
  FetchOutputs& fetch_outputs = *(g_fetch_value->GetMutable<FetchOutputs>());

  size_t size = fetch_outputs.size();
  std::vector<std::vector<T>> result;
  result.reserve(size);
  for (size_t i = 0; i < size; i++) {
    std::vector<T> tmp;
    tmp.resize(fetch_outputs[i].numel());
    memcpy(tmp.data(), fetch_outputs[i].data<T>(),
           fetch_outputs[i].numel() * sizeof(T));
    result.push_back(tmp);
  }

  return result;
}

class ExecutorTesterRandom : public ::testing::Test {
 public:
  virtual void SetUp() override {
    int input_dim = 5, batch_size = 2, embed_dim = 5;

    auto temp_init_root_block = init_pdesc_.add_blocks();
    temp_init_root_block->set_idx(0);
    temp_init_root_block->set_parent_idx(-1);
    paddle::framework::ProgramDescBind& init_program =
        paddle::framework::ProgramDescBind::Instance(&init_pdesc_);
    paddle::framework::BlockDescBind* init_root_block = init_program.Block(0);

    AddOp("gaussian_random", {}, {{"Out", {"w1"}}},
          {{"dims", std::vector<int>{input_dim, embed_dim}}}, init_root_block);
    AddOp("gaussian_random", {}, {{"Out", {"w2"}}},
          {{"dims", std::vector<int>{embed_dim, input_dim}}}, init_root_block);
    AddOp("fetch", {{"Input", {"w1"}}}, {},
          {{"dims", std::vector<int>{input_dim, embed_dim}}, {"col", 0}},
          init_root_block);
    AddOp("fetch", {{"Input", {"w2"}}}, {},
          {{"dims", std::vector<int>{embed_dim, input_dim}}, {"col", 1}},
          init_root_block);

    // flush
    init_program.Proto();

    auto temp_root_block = pdesc_.add_blocks();
    temp_root_block->set_idx(0);
    temp_root_block->set_parent_idx(-1);
    paddle::framework::ProgramDescBind& program =
        paddle::framework::ProgramDescBind::Instance(&pdesc_);
    paddle::framework::BlockDescBind* root_block = program.Block(0);

    AddOp("gaussian_random", {}, {{"Out", {"a"}}},
          {{"dims", std::vector<int>{batch_size, input_dim}}}, root_block);
    AddOp("mul", {{"X", {"a"}}, {"Y", {"w1"}}}, {{"Out", {"b"}}}, {},
          root_block);
    AddOp("mul", {{"X", {"b"}}, {"Y", {"w2"}}}, {{"Out", {"a_out"}}}, {},
          root_block);
    AddOp("squared_l2_distance", {{"X", {"a"}}, {"Y", {"a_out"}}},
          {{"Out", {"l2_distance"}}, {"sub_result", {"l2_distance_sub"}}}, {},
          root_block);

    AddOp("gaussian_random", {}, {{"Out", {"l2_distance@GRAD"}}},
          {{"dims", std::vector<int>{batch_size, 1}}}, root_block);
    AppendBackward(program, {});

    program.Proto();

    for (auto& op : pdesc_.blocks(0).ops()) {
      if (op.type() == "sum") {
        LOG(INFO) << "Here";
        for (auto& var : op.inputs()) {
          for (auto& argu : var.arguments()) {
            LOG(INFO) << var.parameter() << " " << argu;
          }
        }
      }
    }

    AddOp("fetch", {{"Input", {"l2_distance"}}}, {},
          {{"dims", std::vector<int>{batch_size}}, {"col", 1}}, root_block);
    // flush
    program.Proto();

    // TODO(tonyyang-svail):
    //   - Test with Backward
  }

 protected:
  ProgramDesc init_pdesc_;
  ProgramDesc pdesc_;
};

class ExecutorTesterFeedAndFetch : public ::testing::Test {
 public:
  virtual void SetUp() override {
    auto temp_root_block = pdesc_.add_blocks();
    temp_root_block->set_idx(0);
    temp_root_block->set_parent_idx(-1);

    // wrap to BlockDescBind
    paddle::framework::ProgramDescBind& program =
        paddle::framework::ProgramDescBind::Instance(&pdesc_);
    paddle::framework::BlockDescBind* root_block = program.Block(0);

    std::vector<int> dim{6};

    AddOp("feed", {}, {{"Out", {"a"}}}, {{"dims", dim}, {"col", 0}},
          root_block);
    AddOp("feed", {}, {{"Out", {"b"}}}, {{"dims", dim}, {"col", 1}},
          root_block);
    AddOp("fetch", {{"Input", {"a"}}}, {}, {{"dims", dim}, {"col", 0}},
          root_block);
    AddOp("fetch", {{"Input", {"b"}}}, {}, {{"dims", dim}, {"col", 1}},
          root_block);

    // flush
    program.Proto();

    std::vector<float> vec1 = {1.0, 2.0, 3.0, 4.0, 5.0, 6.0};
    std::vector<float> vec2 = {4.0, 5.0, 6.0, 7.0, 8.0, 9.0};
    inputs_.push_back(vec1);
    inputs_.push_back(vec2);
  }

 protected:
  ProgramDesc pdesc_;
  std::vector<std::vector<float>> inputs_;
};

#ifndef PADDLE_WITH_CUDA
TEST_F(ExecutorTesterRandom, CPU) {
  std::vector<Place> places;
  CPUPlace cpu_place;
  places.push_back(cpu_place);

  // We have a global Scope and BuddyAllocator, and we must ensure
  // global BuddyAllocator is initialized before global Scope. Thus,
  // global Scope will deconstruct before BuddyAllocator. Otherwise,
  // "pointer being freed was not allocated" error will appear.
  paddle::memory::Used(cpu_place);

  std::unique_ptr<Executor> executor(new Executor(places));

  executor->Run(init_pdesc_, GetGlobalScope(), 0);
  executor->Run(pdesc_, GetGlobalScope(), 0);
  std::vector<std::vector<float>> result = GetFetchVariable<float>();
}

TEST_F(ExecutorTesterFeedAndFetch, CPU) {
  std::vector<Place> places;
  CPUPlace cpu_place;
  places.push_back(cpu_place);

  // We have a global Scope and BuddyAllocator, and we must ensure
  // global BuddyAllocator is initialized before global Scope. Thus,
  // global Scope will deconstruct before BuddyAllocator. Otherwise,
  // "pointer being freed was not allocated" error will appear.
  paddle::memory::Used(cpu_place);

  std::unique_ptr<Executor> executor(new Executor(places));

  for (int batch_id = 0; batch_id < 3; batch_id++) {
    SetFeedVariable<float>(inputs_);
    executor->Run(pdesc_, GetGlobalScope(), 0);
    std::vector<std::vector<float>> result = GetFetchVariable<float>();
    PADDLE_ENFORCE_EQ(result.size(), inputs_.size());
    for (size_t i = 0; i < result.size(); ++i) {
      PADDLE_ENFORCE_EQ(result[i].size(), inputs_[i].size());
      for (size_t j = 0; j < result[i].size(); ++j) {
        PADDLE_ENFORCE_EQ(result[i][j], inputs_[i][j]);
      }
    }
  }
}
#else
TEST_F(ExecutorTesterRandom, GPU) {
  std::vector<Place> places;
  GPUPlace gpu_place(0);
  places.push_back(gpu_place);

  // We have a global Scope and BuddyAllocator, and we must ensure
  // global BuddyAllocator is initialized before global Scope. Thus,
  // global Scope will deconstruct before BuddyAllocator. Otherwise,
  // "pointer being freed was not allocated" error will appear.
  // If paddle is compiled with GPU, both CPU and GPU BuddyAllocator
  // need to be used at first.
  paddle::memory::Used(CPUPlace());
  paddle::memory::Used(gpu_place);

  std::unique_ptr<Executor> executor(new Executor(places));

  executor->Run(init_pdesc_, GetGlobalScope(), 0);
  executor->Run(pdesc_, GetGlobalScope(), 0);
  std::vector<std::vector<float>> result = GetFetchVariable<float>();
}

TEST_F(ExecutorTesterFeedAndFetch, GPU) {
  std::vector<Place> places;
  GPUPlace gpu_place(0);
  places.push_back(gpu_place);
  // We have a global Scope and BuddyAllocator, and we must ensure
  // global BuddyAllocator is initialized before global Scope. Thus,
  // global Scope will deconstruct before BuddyAllocator. Otherwise,
  // "pointer being freed was not allocated" error will appear.
  // If paddle is compiled with GPU, both CPU and GPU BuddyAllocator
  // need to be used at first.
  paddle::memory::Used(CPUPlace());
  paddle::memory::Used(gpu_place);

  std::unique_ptr<Executor> executor(new Executor(places));

  for (int batch_id = 0; batch_id < 3; batch_id++) {
    SetFeedVariable<float>(inputs_);
    executor->Run(pdesc_, GetGlobalScope(), 0);
    std::vector<std::vector<float>> result = GetFetchVariable<float>();
    PADDLE_ENFORCE_EQ(result.size(), inputs_.size());
    for (size_t i = 0; i < result.size(); ++i) {
      PADDLE_ENFORCE_EQ(result[i].size(), inputs_[i].size());
      for (size_t j = 0; j < result[i].size(); ++j) {
        PADDLE_ENFORCE_EQ(result[i][j], inputs_[i][j]);
      }
    }
  }
}
#endif
