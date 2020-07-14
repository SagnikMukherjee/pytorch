#include <c10/core/TensorOptions.h>
#include <test/cpp/jit/test_base.h>
#include <torch/csrc/autograd/generated/variable_factories.h>
#include <torch/csrc/jit/api/module.h>
#include <torch/csrc/jit/mobile/import.h>
#include <torch/csrc/jit/mobile/module.h>
#include <torch/csrc/jit/serialization/import.h>
#include <torch/torch.h>

// Tests go in torch::jit
namespace torch {
namespace jit {

void testLiteInterpreterParams() {
  Module m("m");
  m.register_parameter("foo", torch::ones({1}, at::requires_grad()), false);
  m.define(R"(
    def forward(self, x):
      b = 1.0
      return self.foo * x + b
  )");
  double learning_rate = 0.1, momentum = 0.1;
  int n_epoc = 10;
  // init: y = x + 1;
  // target: y = 2 x + 1
  std::vector<std::pair<Tensor, Tensor>> trainData{
      {1 * torch::ones({1}), 3 * torch::ones({1})},
  };
  // Reference: Full jit
  std::stringstream ms;
  m.save(ms);
  auto mm = load(ms);
  //  mm.train();
  std::vector<::at::Tensor> parameters;
  for (auto parameter : mm.parameters()) {
    parameters.emplace_back(parameter);
  }
  ::torch::optim::SGD optimizer(
      parameters, ::torch::optim::SGDOptions(learning_rate).momentum(momentum));
  for (int epoc = 0; epoc < n_epoc; ++epoc) {
    for (auto& data : trainData) {
      auto source = data.first, targets = data.second;
      optimizer.zero_grad();
      std::vector<IValue> train_inputs{source};
      auto output = mm.forward(train_inputs).toTensor();
      auto loss = ::torch::l1_loss(output, targets);
      loss.backward();
      optimizer.step();
    }
  }
  std::stringstream ss;
  m._save_for_mobile(ss);
  mobile::Module bc = _load_for_mobile(ss);
  std::vector<::at::Tensor> bc_parameters = bc.parameters();
  ::torch::optim::SGD bc_optimizer(
      bc_parameters,
      ::torch::optim::SGDOptions(learning_rate).momentum(momentum));
  for (int epoc = 0; epoc < n_epoc; ++epoc) {
    for (auto& data : trainData) {
      auto source = data.first, targets = data.second;
      bc_optimizer.zero_grad();
      std::vector<IValue> train_inputs{source};
      auto output = bc.forward(train_inputs).toTensor();
      auto loss = ::torch::l1_loss(output, targets);
      loss.backward();
      bc_optimizer.step();
    }
  }
  AT_ASSERT(parameters[0].item<float>() == bc_parameters[0].item<float>());
}

void testMobileNamedParameters() {
  Module m("m");
  m.register_parameter("foo", torch::ones({}), false);
  m.register_parameter("foo2", 2 * torch::ones({}), false);
  m.define(R"(
    def add_it(self, x):
      b = 4
      return self.foo + x + b
  )");
  Module child("m2");
  child.register_parameter("foo", 4 * torch::ones({}), false);
  m.register_module("child", child);

  std::vector<IValue> values;
  for (auto e : m.named_parameters()) {
    values.push_back(e.value);
  }

  std::stringstream ss;
  m._save_for_mobile(ss);
  mobile::Module bc = _load_for_mobile(ss);

  std::vector<IValue> mobile_values;
  auto mobile_params = bc.named_parameters();
  for (auto it = mobile_params.begin(); it != mobile_params.end(); it++) {
    mobile_values.push_back(it->value());
  }
  AT_ASSERT(values == mobile_values);
}

void testMobileSaveLoadData() {
  Module m("m");
  m.register_parameter("foo", torch::ones({}), false);
  m.register_parameter("foo2", 2 * torch::ones({}), false);
  m.define(R"(
    def add_it(self, x):
      b = 4
      return self.foo + x + b
  )");
  Module child("m2");
  child.register_parameter("foo", 4 * torch::ones({}), false);
  m.register_module("child", child);

  std::vector<IValue> values;
  for (auto e : m.named_parameters()) {
    values.emplace_back(e.value);
  }
  std::stringstream ss;
  std::stringstream ss_data;
  m._save_for_mobile(ss);
  mobile::Module bc = _load_for_mobile(ss);
  bc.save_data(ss_data);
  auto mobile_params = _load_mobile_data(ss_data);
  std::vector<IValue> mobile_values;
  for (const auto& e : mobile_params) {
    mobile_values.emplace_back(e);
  }
  AT_ASSERT(values == mobile_values);
}

} // namespace jit
} // namespace torch