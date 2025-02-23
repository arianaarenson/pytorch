#include <test/cpp/jit/test_utils.h>

#include <gtest/gtest.h>

#include <c10/core/TensorOptions.h>
#include <torch/csrc/autograd/generated/variable_factories.h>
#include <torch/csrc/jit/api/module.h>
#include <torch/csrc/jit/frontend/resolver.h>
#include <torch/csrc/jit/mobile/backport.h>
#include <torch/csrc/jit/mobile/backport_manager.h>
#include <torch/csrc/jit/mobile/import.h>
#include <torch/csrc/jit/mobile/model_compatibility.h>
#include <torch/csrc/jit/mobile/module.h>
#include <torch/csrc/jit/mobile/runtime_compatibility.h>
#include <torch/csrc/jit/serialization/export.h>
#include <torch/csrc/jit/serialization/import.h>
#include <torch/custom_class.h>
#include <torch/torch.h>

#include <unordered_set>

// Tests go in torch::jit
namespace torch {
namespace jit {

TEST(LiteInterpreterTest, UpsampleNearest2d) {
  Module m("m");
  m.define(R"(
    def forward(self, input: Tensor, scale:float):
      return torch.upsample_nearest2d(input, [1, 1], float(scale), float(scale))
  )");

  std::vector<IValue> inputs;
  inputs.emplace_back(torch::rand({1, 3, 128, 128}));
  inputs.emplace_back(at::Scalar(2.0));
  auto ref = m.forward(inputs);

  std::stringstream ss;
  m._save_for_mobile(ss);
  mobile::Module bc = _load_for_mobile(ss);
  IValue res;
  res = bc.forward(inputs);

  auto resd = res.toTensor();
  auto refd = ref.toTensor();
  ASSERT_TRUE(resd.equal(refd));
}

TEST(LiteInterpreterTest, CheckAttrAccess) {
  Module m("m");
  m.register_attribute("mobile_optimized", BoolType::get(), true);

  std::stringstream ss;
  m._save_for_mobile(ss);
  mobile::Module bc = _load_for_mobile(ss);
  bool mobile_optimized = bc.attr("mobile_optimized", false).toBool();

  AT_ASSERT(mobile_optimized);
  m.setattr("mobile_optimized", false);
  ss = std::stringstream();
  m._save_for_mobile(ss);
  bc = _load_for_mobile(ss);
  mobile_optimized = bc.attr("mobile_optimized", false).toBool();

  AT_ASSERT(!mobile_optimized);
}

TEST(LiteInterpreterTest, MethodInvocation) { // NOLINT (use =delete in gtest)
  const std::vector<std::string> test_programs{
      // test invoking a method with default parameter
      R"(
      def test_func(self, x, b : int = 4):
        return self.foo + x + b
      )",
      // inner method call with default parameter (gets inlined)
      R"(
      def add_with_default_arg(self, x, b : int = 4):
        return self.foo + x + b
      def test_func(self, x):
        return self.add_with_default_arg(x)  # invoke method w/ default arg
      )",
      // simple method call
      R"(
      def test_func(self, x):
        b = 4
        return self.foo + x + b
      )",
  };
  for (const auto& test_program : test_programs) {
    Module m("m");
    m.register_parameter("foo", torch::ones({}), false);
    m.define(test_program);

    const int fortyTwo = 42; // (keep linter happy)
    auto minput = fortyTwo * torch::ones({});
    auto ref = m.run_method("test_func", minput);

    std::stringstream ss;
    m._save_for_mobile(ss);
    mobile::Module bc = _load_for_mobile(ss);
    const auto& test_func = bc.get_method("test_func");
    IValue res;
    for (int i = 0; i < 3; ++i) {
      res = test_func({minput});
    }

    auto resd = res.toTensor().item<float>();
    auto refd = ref.toTensor().item<float>();
    AT_ASSERT(resd == refd);
  }
}

TEST(LiteInterpreterTest, Conv) {
  auto s = std::getenv("PYTORCH_TEST_WITH_TSAN");
  if (s && strcmp(s, "1") == 0)
    return;

  std::vector<torch::jit::IValue> inputs;

  Module m("m");
  m.register_parameter("weight", torch::ones({20, 1, 5, 5}), false);
  m.register_parameter("bias", torch::ones({20}), false);
  m.define(R"(
    def forward(self, input):
      return torch._convolution(input, self.weight, self.bias, [1, 1], [0, 0], [1, 1], False, [0, 0], 1, False, False, True, True)
  )");

  // NOLINTNEXTLINE(cppcoreguidelines-avoid-magic-numbers,modernize-use-emplace)
  inputs.push_back(torch::ones({1, 1, 28, 28}));

  auto outputref = m.forward(inputs).toTensor();

  std::stringstream ss;
  m._save_for_mobile(ss);
  mobile::Module bc = _load_for_mobile(ss);
  IValue res;
  for (int i = 0; i < 3; ++i) {
    res = bc.get_method("forward")(inputs);
  }
  auto output = res.toTensor();
  AT_ASSERT(outputref.dim() == output.dim());
  AT_ASSERT(
      outputref[0][0][0][0].item<int>() == output[0][0][0][0].item<int>());
}

TEST(LiteInterpreterTest, Inline) {
  Module m("m");
  m.define(R"JIT(
  def foo1(self, x):
      return x + 1

  def foo2(self, x):
      return self.foo1(x) + 2

  def foo3(self, x):
      return self.foo2(x) + 3
  )JIT");
  std::stringstream ss;
  m._save_for_mobile(ss);
  mobile::Module bc = _load_for_mobile(ss);
  std::vector<torch::jit::IValue> inputs({torch::ones({})});
  auto output = bc.get_method("foo3")(inputs);
  AT_ASSERT(output.toTensor().item<float>() == 7.0);
}

TEST(LiteInterpreterTest, Tuple) {
  Module m("m");
  m.define(R"JIT(
  def foo(self, x):
      return (1, 2, x + 3)

  def forward(self, x):
      tuple = self.foo(x)
      return tuple
  )JIT");
  std::stringstream ss;
  m._save_for_mobile(ss);
  mobile::Module bc = _load_for_mobile(ss);
  std::vector<torch::jit::IValue> inputs({torch::ones({})});
  auto output = bc.get_method("forward")(inputs);
  AT_ASSERT(output.toTuple()->elements()[1].toInt() == 2);
}

TEST(LiteInterpreterTest, Dict) {
  Module m("m");
  m.define(R"JIT(
  def foo(self, x):
      return {"result": x + 1}

  def forward(self, x):
      d = self.foo(x)
      return d
  )JIT");
  std::stringstream ss;
  m._save_for_mobile(ss);
  mobile::Module bc = _load_for_mobile(ss);
  std::vector<torch::jit::IValue> inputs({torch::ones({})});
  auto output = bc.get_method("forward")(inputs);
  AT_ASSERT(output.toGenericDict().at("result").toTensor().item().toInt() == 2);
}

TEST(LiteInterpreterTest, PrimOverload) {
  /*
  // temporarily disabled
  script::Module m("m");
  m.define(R"JIT(
  def forward(self, x):
      result = [1, 2]
      result.append(3)
      return result
  )JIT");
  std::stringstream ss;
  m._save_for_mobile(ss);
  mobile::Module bc = _load_for_mobile(ss);
  std::vector<torch::jit::IValue> inputs({torch::ones({})});
  auto output = bc.get_method("forward")(inputs);
  AT_ASSERT(output.toIntList()[2] == 3);
  */
}

TEST(LiteInterpreterTest, Prim) {
  Module m("m");
  m.define(R"JIT(
        def forward(self, x):
            return int(x)
  )JIT");

  std::vector<IValue> inputs;
  auto minput = 3.5 * torch::ones({});
  inputs.emplace_back(minput);
  auto ref = m.run_method("forward", minput);

  std::stringstream ss;
  m._save_for_mobile(ss);
  mobile::Module bc = _load_for_mobile(ss);
  IValue res;
  for (int i = 0; i < 3; ++i) {
    // NOLINTNEXTLINE(performance-unnecessary-copy-initialization)
    auto bcinputs = inputs;
    res = bc.get_method("forward")(bcinputs);
  }

  auto resi = res.toInt();
  auto refi = ref.toInt();
  AT_ASSERT(resi == refi);
}

TEST(LiteInterpreterTest, PrimScalar) {
  Module m("m");
  m.define(R"JIT(
        def forward(self, x):
            return int(x.item())
  )JIT");

  std::vector<IValue> inputs;
  auto minput = 3.5 * torch::ones({});
  inputs.emplace_back(minput);
  auto ref = m.run_method("forward", minput);

  std::stringstream ss;
  m._save_for_mobile(ss);
  mobile::Module bc = _load_for_mobile(ss);
  IValue res;
  for (int i = 0; i < 3; ++i) {
    // NOLINTNEXTLINE(performance-unnecessary-copy-initialization)
    auto bcinputs = inputs;
    res = bc.get_method("forward")(bcinputs);
  }

  auto resi = res.toInt();
  auto refi = ref.toInt();
  AT_ASSERT(resi == refi);
}

TEST(LiteInterpreterTest, LoadOrigJit) {
  Module m("m");
  m.register_parameter("foo", torch::ones({}), false);
  m.define(R"(
    def forward(self, x):
      b = 4
      return self.foo + x + b
  )");
  std::stringstream ss;
  m.save(ss);
  ASSERT_THROWS_WITH_MESSAGE(_load_for_mobile(ss), "file not found");
}

TEST(LiteInterpreterTest, WrongMethodName) {
  Module m("m");
  m.register_parameter("foo", torch::ones({}), false);
  m.define(R"(
    def add(self, x):
      b = 4
      return self.foo + x + b
  )");
  std::stringstream ss;
  m._save_for_mobile(ss);
  mobile::Module bc = _load_for_mobile(ss);
  std::vector<IValue> inputs;
  auto minput = 5 * torch::ones({});
  inputs.emplace_back(minput);
  ASSERT_THROWS_WITH_MESSAGE(
      bc.get_method("forward")(inputs), "is not defined");
}

TEST(LiteInterpreterTest, SetState) {
  Module m("m");
  m.register_parameter("foo", torch::ones({}), false);
  m.define(R"(
    def __getstate__(self):
      return self.foo + self.foo
    def __setstate__(self, a):
      self.foo = a
    def forward(self, x):
      b = 4
      return self.foo + x + b
  )");

  std::vector<IValue> inputs;
  auto minput = 5 * torch::ones({});
  inputs.emplace_back(minput);

  std::stringstream ms;
  m.save(ms);
  auto loaded_m = load(ms);
  auto ref = loaded_m.run_method("forward", minput);

  std::stringstream ss;
  m._save_for_mobile(ss);
  mobile::Module bc = _load_for_mobile(ss);
  IValue res;
  for (int i = 0; i < 3; ++i) {
    // NOLINTNEXTLINE(performance-unnecessary-copy-initialization)
    auto bcinputs = inputs;
    res = bc.get_method("forward")(bcinputs);
  }

  auto resd = res.toTensor().item<float>();
  auto refd = ref.toTensor().item<float>();
  AT_ASSERT(resd == refd);
}

class TorchBindLiteInterpreterTestStruct
    : public torch::jit::CustomClassHolder {
 public:
  std::string get(at::Tensor t) {
    std::stringstream ss;
    ss << "Hello! Your tensor has ";
    ss << t.numel();
    ss << " elements!";
    return ss.str();
  }
};

namespace {
struct ClassNamespaceValue : public SugaredValue {
  explicit ClassNamespaceValue(c10::QualifiedName name)
      : basename_(std::move(name)) {}

  std::shared_ptr<SugaredValue> attr(
      const SourceRange& loc,
      Function& m,
      const std::string& name) override {
    const auto fullName = c10::QualifiedName(basename_, name);

    // Check to see if it is a custom class.
    if (auto custom_class = getCustomClass(fullName.qualifiedName())) {
      return std::make_shared<ClassValue>(custom_class);
    }

    // If it's not a custom class, assume it's another namespace
    // NOLINTNEXTLINE(performance-move-const-arg)
    return std::make_shared<ClassNamespaceValue>(std::move(fullName));
  }

  std::string kind() const override {
    return "Class Namespace";
  }

 private:
  c10::QualifiedName basename_;
};

struct TestModuleResolver : public Resolver {
  std::shared_ptr<SugaredValue> resolveValue(
      const std::string& name,
      Function& m,
      const SourceRange& loc) override {
    if (name == "torch") {
      return std::make_shared<BuiltinModule>("aten");
    } else if (name == "__torch__") {
      return std::make_shared<ClassNamespaceValue>(c10::QualifiedName(name));
    }

    return nullptr;
  }

  TypePtr resolveType(const std::string& name, const SourceRange& loc)
      override {
    return nullptr;
  }
};
} // namespace

TEST(LiteInterpreterTest, BuiltinClass) {
  script::Module m("m");

  auto cls = getCustomClass(
      "__torch__.torch.classes._TorchScriptTesting._LiteInterpreterTest");
  TORCH_INTERNAL_ASSERT(cls);
  c10::intrusive_ptr<torch::CustomClassHolder> obj_holder;
  m.register_attribute("my_obj", cls, IValue::make_capsule(obj_holder));

  m.register_parameter("foo", torch::ones({}), false);
  m.define(
      R"(
    def __getstate__(self):
      return 1
    def __setstate__(self, a):
      self.my_obj = __torch__.torch.classes._TorchScriptTesting._LiteInterpreterTest()

    def forward(self, x) -> str:
      return self.my_obj.get(x)
  )",
      std::make_shared<TestModuleResolver>());

  std::stringstream ss;
  m._save_for_mobile(ss);
  mobile::Module bc = _load_for_mobile(ss);
  auto res =
      bc.get_method("forward")(std::vector<IValue>{torch::zeros({3, 4})});
  const auto& str = res.toStringRef();
  std::string expected = "Hello! Your tensor has 12 elements!";
  AT_ASSERT(str == expected);
}

TEST(LiteInterpreterTest, BuiltinFunction) {
  script::Module m("m");
  auto custom_class_obj =
      make_custom_class<TorchBindLiteInterpreterTestStruct>();
  m.register_attribute("my_obj", custom_class_obj.type(), custom_class_obj);
  m.define(R"(
    def forward(self, x) -> str:
      return self.my_obj.get(x)
  )");

  std::stringstream ss;
  m._save_for_mobile(ss);
  mobile::Module bc = _load_for_mobile(ss);
  auto res =
      bc.get_method("forward")(std::vector<IValue>{torch::zeros({3, 4})});
  // NOLINTNEXTLINE(performance-unnecessary-copy-initialization)
  auto str = res.toStringRef();
  std::string expected = "Hello! Your tensor has 12 elements!";
  AT_ASSERT(str == expected);
}

#if !defined FB_XPLAT_BUILD
TEST(LiteInterpreterTest, ModuleInfoBasic) {
  Module m("M");
  m.define(R"JIT(
    def forward(self, x):
      return 2 * x
  )JIT");

  std::stringstream ss;
  m._save_for_mobile(ss, {}, true);
  mobile::Module bc = _load_for_mobile(ss);

  std::unordered_set<std::string> module_debug_info_set;
  size_t pc = 0;
  while (true) {
    try {
      std::string module_info = bc.get_forward_method_debug_info(pc);
      if (!module_info.empty() &&
          (module_info.find("debug_handle") == std::string::npos)) {
        module_debug_info_set.insert(module_info);
      }
      ++pc;
    } catch (const std::exception& e) {
      break;
    }
  }

  AT_ASSERT(module_debug_info_set.count("top(M)::<unknown>.aten::mul"));
}

TEST(LiteInterpreterTest, NotSaveModuleInfo) {
  Module m("M");
  m.define(R"JIT(
    def forward(self, x):
      return x + 5
  )JIT");

  std::stringstream ss;
  m._save_for_mobile(ss);
  mobile::Module bc = _load_for_mobile(ss);

  size_t pc = 0;
  while (true) {
    try {
      std::string module_info = bc.get_forward_method_debug_info(pc);
      AT_ASSERT(
          module_info.empty() ||
          (module_info.find("debug_handle") != std::string::npos));
      ++pc;
    } catch (const std::exception& e) {
      break;
    }
  }
}

TEST(LiteInterpreterTest, OneSubmoduleModuleInfo) {
  Module a("A");
  a.define(R"JIT(
    def forward(self, x):
      return 2 * x + 5
  )JIT");
  Module b("B");
  b.register_module("A0", a);
  b.define(R"JIT(
    def forward(self, x):
      return self.A0.forward(x) + 1
  )JIT");

  std::stringstream ss;
  b._save_for_mobile(ss, {}, true);
  mobile::Module bc = _load_for_mobile(ss);

  std::set<std::string> module_debug_info_set;
  size_t pc = 0;
  while (true) {
    try {
      std::string module_info = bc.get_forward_method_debug_info(pc);
      if (!module_info.empty() &&
          (module_info.find("debug_handle") == std::string::npos)) {
        module_debug_info_set.insert(module_info);
      }
      ++pc;
    } catch (const std::exception& e) {
      break;
    }
  }

  AT_ASSERT(module_debug_info_set.count("top(B)::<unknown>.aten::add"));
  AT_ASSERT(module_debug_info_set.count(
      "top(B)::<unknown>.A0(A)::forward.aten::add"));
  AT_ASSERT(module_debug_info_set.count(
      "top(B)::<unknown>.A0(A)::forward.aten::mul"));
}

TEST(LiteInterpreterTest, TwoSubmodulesModuleInfo) {
  Module a("A");
  a.define(R"JIT(
    def forward(self, x):
      return x + 1
  )JIT");
  Module b("B");
  b.define(R"JIT(
    def forward(self, x):
      return x + 2
  )JIT");
  Module c("C");
  c.register_module("A0", a);
  c.register_module("B0", b);
  c.define(R"JIT(
    def forward(self, x):
      return self.A0.forward(x) + self.B0.forward(x)
  )JIT");

  std::stringstream ss;
  c._save_for_mobile(ss, {}, true);
  mobile::Module bc = _load_for_mobile(ss);

  std::set<std::string> module_debug_info_set;
  size_t pc = 0;
  while (true) {
    try {
      std::string module_info = bc.get_forward_method_debug_info(pc);
      if (!module_info.empty() &&
          (module_info.find("debug_handle") == std::string::npos)) {
        module_debug_info_set.insert(module_info);
      }
      ++pc;
    } catch (const std::exception& e) {
      break;
    }
  }

  AT_ASSERT(module_debug_info_set.count("top(C)::<unknown>.aten::add"));
  AT_ASSERT(module_debug_info_set.count(
      "top(C)::<unknown>.A0(A)::forward.aten::add"));
  AT_ASSERT(module_debug_info_set.count(
      "top(C)::<unknown>.B0(B)::forward.aten::add"));
}

TEST(LiteInterpreterTest, GetRuntimeByteCodeVersion) {
  auto runtime_bytecode_version = _get_runtime_bytecode_version();
  AT_ASSERT(
      runtime_bytecode_version ==
      caffe2::serialize::kMaxSupportedBytecodeVersion);
}

/**
 * The test below is disarmed for FB internal xplat builds since
 * BUCK requires us to pass in the script_module_v4.ptl file in
 * as a resource dependency of the build rule for this file, and
 * we would need to access it via the C++ Resources API instead
 * of directly reading from disk (which is what the open source
 * build/run does).
 */
TEST(LiteInterpreterTest, GetByteCodeVersion) {
  std::string filePath(__FILE__);
  auto test_model_file_v4 =
      filePath.substr(0, filePath.find_last_of("/\\") + 1);
  test_model_file_v4.append("script_module_v4.ptl");

  auto version_v4 = _get_model_bytecode_version(test_model_file_v4);
  AT_ASSERT(version_v4 == 4);
}
#endif // !defined(FB_XPLAT_BUILD)

namespace {

void compareModelOutput(
    const std::vector<IValue>& actual_result_list,
    const std::vector<Tensor>& expect_result_list) {
  AT_ASSERT(actual_result_list.size() == expect_result_list.size());
  AT_ASSERT(actual_result_list[0].toTensor().equal(expect_result_list[0]));
  AT_ASSERT(
      actual_result_list[1].toTensor().dim() == expect_result_list[1].dim());
  AT_ASSERT(actual_result_list[2].toTensor().equal(expect_result_list[2]));
}

void runAndCheckTorchScriptModel(
    std::stringstream& input_model_stream,
    const std::vector<IValue>& input_data,
    const std::vector<Tensor>& expect_result_list,
    const int64_t expect_version) {
  auto actual_version = _get_model_bytecode_version(input_model_stream);
  AT_ASSERT(actual_version == expect_version);

  // Load and run the backport model, then compare the result with expect
  // result
  Module m_mobile = load(input_model_stream);

  auto actual_result = m_mobile.forward(input_data);
  std::vector<IValue> actual_result_list = actual_result.toTuple()->elements();
  compareModelOutput(actual_result_list, expect_result_list);
}

void runAndCheckBytecodeModel(
    std::stringstream& input_model_stream,
    const std::vector<IValue>& input_data,
    const std::vector<Tensor>& expect_result_list,
    const int64_t expect_version) {
  auto actual_version = _get_model_bytecode_version(input_model_stream);
  AT_ASSERT(actual_version == expect_version);

  // Load and run the backport model, then compare the result with expect
  // result
  Module m_mobile = load(input_model_stream);

  auto actual_result = m_mobile.forward(input_data);
  std::vector<IValue> actual_result_list = actual_result.toTuple()->elements();

  compareModelOutput(actual_result_list, expect_result_list);
}

void backportAllVersionCheck(
    std::stringstream& test_model_file_stream,
    std::vector<IValue>& input_data,
    std::vector<Tensor>& expect_result_list,
    const int64_t expect_from_version) {
  auto from_version = _get_model_bytecode_version(test_model_file_stream);
  AT_ASSERT(from_version == expect_from_version);

  // Backport script_module_v5.ptl to an older version
  constexpr int64_t minimum_to_version = 4;
  int64_t current_to_version = from_version - 1;

  // Verify all candidate to_version work as expected. All backport to version
  // larger than minimum_to_version should success.
  while (current_to_version >= minimum_to_version) {
    // Do not declare std::stringstream oss outside of the while loop as
    // oss.clear() doesn't reset the stream content, only clears out error state
    // flag in stringstream causing a problematic stream. Instead, it's cleaner
    // and safer to just declare a new std::stringstream one and swap them.
    std::stringstream oss;
    bool backPortSuccess =
        _backport_for_mobile(test_model_file_stream, oss, current_to_version);
    AT_ASSERT(backPortSuccess);

    // Check backport model version
    auto backport_version = _get_model_bytecode_version(oss);
    AT_ASSERT(backport_version == current_to_version);

    // Load and run the backport model, then compare the result with expect
    // result
    runAndCheckBytecodeModel(
        oss, input_data, expect_result_list, current_to_version);
    runAndCheckTorchScriptModel(
        oss, input_data, expect_result_list, current_to_version);

    current_to_version--;
  }
  //  backport to minimum version - 1 should fail
  std::stringstream oss;
  bool backPortSuccess =
      _backport_for_mobile(test_model_file_stream, oss, minimum_to_version - 1);
  AT_ASSERT(!backPortSuccess);
}
} // namespace

#if !defined FB_XPLAT_BUILD
TEST(LiteInterpreterTest, BackPortByteCodeModelAllVersions) {
  torch::jit::Module module("m");
  // NOLINTNEXTLINE(cppcoreguidelines-avoid-magic-numbers)
  module.register_parameter("weight", torch::ones({20, 1, 5, 5}), false);
  // NOLINTNEXTLINE(cppcoreguidelines-avoid-magic-numbers)
  module.register_parameter("bias", torch::ones({20}), false);
  module.define(R"(
    def forward(self, input):
      x1 = torch.zeros(2, 2)
      x2 = torch.empty_like(torch.empty(2, 2))
      x3 = torch._convolution(input, self.weight, self.bias, [1, 1], [0, 0], [1, 1], False, [0, 0], 1, False, False, True, True)
      return (x1, x2, x3)
  )");

  torch::jit::Module module_freeze = freeze(module);

  std::stringstream input_model_stream;
  module_freeze._save_for_mobile(input_model_stream);
  std::vector<IValue> input_data =
      std::vector<IValue>({torch::ones({1, 1, 28, 28})});
  std::vector<Tensor> expect_result_list;
  expect_result_list.emplace_back(at::ones({2, 2}, ScalarType::Float) * 0);
  expect_result_list.emplace_back(at::ones({2, 2}, ScalarType::Float));
  expect_result_list.emplace_back(
      at::ones({1, 20, 24, 24}, ScalarType::Float) * 26);
  backportAllVersionCheck(
      input_model_stream,
      input_data,
      expect_result_list,
      caffe2::serialize::kProducedBytecodeVersion);
}
#endif // !defined(FB_XPLAT_BUILD)

TEST(LiteInterpreterTest, GetRuntimeOpsAndInfo) {
  auto runtime_ops = _get_runtime_ops_and_info();
  // Ballpark estimate of the minimal number of ops; just used to
  // verify API returns a reasonably large number.
  AT_ASSERT(runtime_ops.size() > 2900);
}

TEST(LiteInterpreterTest, isCompatibleSuccess) {
  // test trivial success case
  auto runtime_info = RuntimeCompatibilityInfo::get();
  std::unordered_map<std::string, OperatorInfo> model_ops;
  model_ops["aten::add.Scalar"] = OperatorInfo{2};

  auto model_info = ModelCompatibilityInfo{
      caffe2::serialize::kMaxSupportedBytecodeVersion, model_ops};

  AT_ASSERT(
      is_compatible(runtime_info, model_info).status ==
      ModelCompatibilityStatus::OK);
}

TEST(LiteInterpreterTest, isCompatibleFail) {
  // test trivial failure due to ops
  std::unordered_map<std::string, OperatorInfo> model_ops;
  model_ops["aten::add.Scalar"] = OperatorInfo{2};
  auto model_info = ModelCompatibilityInfo{
      caffe2::serialize::kMaxSupportedBytecodeVersion, model_ops};
  std::unordered_map<std::string, OperatorInfo> runtime_ops;
  runtime_ops["aten::add.Int"] = OperatorInfo{2};
  auto runtime_info = RuntimeCompatibilityInfo{
      caffe2::serialize::kMaxSupportedBytecodeVersion, runtime_ops};

  auto result = is_compatible(runtime_info, model_info);
  AT_ASSERT(result.status = ModelCompatibilityStatus::ERROR);
  AT_ASSERT(
      result.errors[0] ==
      "Operator 'aten::add.Scalar' missing from runtime (not found)");

  // test trivial failure due to bytecode
  runtime_ops["aten::add.Scalar"] = OperatorInfo{2};
  runtime_info = RuntimeCompatibilityInfo{
      caffe2::serialize::kMaxSupportedBytecodeVersion, runtime_ops};
  model_info.bytecode_version =
      caffe2::serialize::kMaxSupportedBytecodeVersion + 1;

  result = is_compatible(runtime_info, model_info);
  AT_ASSERT(result.status = ModelCompatibilityStatus::ERROR);
}

#if !defined FB_XPLAT_BUILD
// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
TEST(LiteInterpreterTest, SequentialModuleInfo) {
  Module a("A");
  a.define(R"JIT(
    def forward(self, x):
      return x + 1
  )JIT");
  Module b("B");
  b.define(R"JIT(
    def forward(self, x):
      return x + 2
  )JIT");
  Module c("C");
  c.register_module("A0", a);
  c.register_module("B0", b);
  c.define(R"JIT(
    def forward(self, x):
      return self.A0.forward(self.B0.forward(x))
  )JIT");

  std::stringstream ss;
  c._save_for_mobile(ss, {}, true);
  mobile::Module bc = _load_for_mobile(ss);

  std::set<std::string> module_debug_info_set;
  size_t pc = 0;
  while (true) {
    try {
      std::string module_info = bc.get_forward_method_debug_info(pc);
      if (!module_info.empty() &&
          (module_info.find("debug_handle") == std::string::npos)) {
        module_debug_info_set.insert(module_info);
      }
      ++pc;
    } catch (const std::exception& e) {
      break;
    }
  }

  // class A(nn.Module):
  //   def __init__(self):
  //     super(A, self).__init__()

  //   def forward(self, x):
  //     return x + 1

  // class B(nn.Module):
  //   def __init__(self):
  //     super(B, self).__init__()

  //   def forward(self, x):
  //     return x + 2

  // class C(nn.Module):
  //   def __init__(self):
  //     super(C, self).__init__()
  //     self.A0 = A()
  //     self.B0 = B()

  //   def forward(self, x):
  //     return self.A0.forward(self.B0.forward(x))

  AT_ASSERT(module_debug_info_set.count("top(C)::<unknown>.prim::Return"));
  AT_ASSERT(module_debug_info_set.count(
      "top(C)::<unknown>.A0(A)::forward.aten::add"));
  AT_ASSERT(module_debug_info_set.count(
      "top(C)::<unknown>.B0(B)::forward.aten::add"));
}

TEST(LiteInterpreterTest, HierarchyModuleInfo) {
  Module a("A");
  a.define(R"JIT(
    def forward(self, x):
      return x + 1
  )JIT");
  Module b("B");
  b.register_module("A0", a);
  b.define(R"JIT(
    def forward(self, x):
      return self.A0.forward(x) + 1
  )JIT");
  Module c("C");
  c.register_module("B0", b);
  c.define(R"JIT(
    def forward(self, x):
      return self.B0.forward(x) + 1
  )JIT");

  std::stringstream ss;
  c._save_for_mobile(ss, {}, true);
  mobile::Module bc = _load_for_mobile(ss);

  std::set<std::string> module_debug_info_set;
  size_t pc = 0;
  while (true) {
    try {
      std::string module_info = bc.get_forward_method_debug_info(pc);
      if (!module_info.empty() &&
          (module_info.find("debug_handle") == std::string::npos)) {
        module_debug_info_set.insert(module_info);
      }
      ++pc;
    } catch (const std::exception& e) {
      break;
    }
  }

  // There are 3 module information strings here.
  // "top(C).forward": for the add operator in top.
  // "top(C).B0(B).forward": for the add operator in B0.
  // "top(C).B0(B).forward.A0(A).forward": for the add operator in A0.
  AT_ASSERT(module_debug_info_set.count("top(C)::<unknown>.aten::add"));
  AT_ASSERT(module_debug_info_set.count(
      "top(C)::<unknown>.B0(B)::forward.aten::add"));
  AT_ASSERT(module_debug_info_set.count(
      "top(C)::<unknown>.B0(B)::forward.A0(A)::forward.aten::add"));
}

TEST(LiteInterpreterTest, DuplicatedClassTypeModuleInfo) {
  Module a("A");
  a.define(R"JIT(
    def forward(self, x):
      return x + 5
  )JIT");
  Module b("B");
  b.register_module("A0", a);
  b.register_module("A1", a);
  b.define(R"JIT(
    def forward(self, x):
      return self.A0.forward(x) + self.A1.forward(x)
  )JIT");

  std::stringstream ss;
  b._save_for_mobile(ss, {}, true);
  mobile::Module bc = _load_for_mobile(ss);

  std::set<std::string> module_debug_info_set;
  size_t pc = 0;
  while (true) {
    try {
      std::string module_info = bc.get_forward_method_debug_info(pc);
      if (!module_info.empty() &&
          (module_info.find("debug_handle") == std::string::npos)) {
        module_debug_info_set.insert(module_info);
      }
      ++pc;
    } catch (const std::exception& e) {
      break;
    }
  }

  // class A(nn.Module):
  //   def __init__(self):
  //     super(A, self).__init__()

  //   def forward(self, x):
  //     return x + 5

  // class B(nn.Module):
  //   def __init__(self):
  //     super(B, self).__init__()
  //     self.A0 = A()
  //     self.A1 = A()

  //   def forward(self, x):
  //     return self.A0.forward(x) + self.A1.forward(x)

  // There are 3 module information strings here.
  // "top(B).forward": for the add operator in top.
  // "top(B).A0(A).forward": for the add operator in A0.
  // "top(B).A1(A).forward": for the add operator in A1.

  AT_ASSERT(module_debug_info_set.count("top(B)::<unknown>.aten::add"));
  AT_ASSERT(module_debug_info_set.count(
      "top(B)::<unknown>.A0(A)::forward.aten::add"));
  AT_ASSERT(module_debug_info_set.count(
      "top(B)::<unknown>.A1(A)::forward.aten::add"));
}
#endif // !defined(FB_XPLAT_BUILD)

TEST(LiteInterpreterTest, Eval) {
  std::vector<torch::jit::IValue> inputs;

  Module m("m");
  m.define(R"(
    def __init__(self, x):
      self.training = True

    def forward(self, input):
      return torch.dropout(input, 1.0, self.training)
  )");

  // NOLINTNEXTLINE(cppcoreguidelines-avoid-magic-numbers,modernize-use-emplace)
  inputs.push_back(torch::ones({1, 1, 28, 28}));
  m.eval();
  auto outputref = m.forward(inputs).toTensor();

  // save m in training mode to make sure that mobile eval() will correctly
  // change back to eval mode
  m.train();
  std::stringstream ss;
  m._save_for_mobile(ss);
  mobile::Module bc = _load_for_mobile(ss);
  bc.eval();
  IValue res;
  for (int i = 0; i < 3; ++i) {
    res = bc.get_method("forward")(inputs);
  }
  auto output = res.toTensor();
  AT_ASSERT(outputref.dim() == output.dim());
  AT_ASSERT(
      outputref[0][0][0][0].item<int>() == output[0][0][0][0].item<int>());
}

TEST(LiteInterpreterTest, FindWrongMethodName) {
  Module m("m");
  m.register_parameter("foo", torch::ones({}), false);
  m.define(R"(
    def add(self, x):
      b = 4
      return self.foo + x + b
  )");
  std::stringstream ss;
  m._save_for_mobile(ss);
  mobile::Module bc = _load_for_mobile(ss);
  ASSERT_TRUE(bc.find_method("forward") == c10::nullopt);
}

TEST(LiteInterpreterTest, FindAndRunMethod) {
  Module m("m");
  m.register_parameter("foo", torch::ones({}), false);
  m.define(R"(
    def add_it(self, x):
      b = 4
      return self.foo + x + b
  )");

  std::vector<IValue> inputs;
  auto minput = 5 * torch::ones({});
  inputs.emplace_back(minput);
  auto ref = m.get_method("add_it")(inputs);

  std::stringstream ss;
  m._save_for_mobile(ss);
  mobile::Module bc = _load_for_mobile(ss);
  IValue res;
  for (int i = 0; i < 3; ++i) {
    auto bcinputs = inputs;
    auto method = bc.find_method("add_it");
    AT_ASSERT(method != c10::nullopt);
    res = (*method)(std::move(bcinputs));
  }

  auto resd = res.toTensor().item<float>();
  auto refd = ref.toTensor().item<float>();
  AT_ASSERT(resd == refd);
}

TEST(LiteInterpreterTest, RunMethodVariadic) {
  Module m("m");
  m.register_parameter("foo", torch::ones({}), false);
  m.define(R"(
    def add_three(self, x, y):
      return self.foo + x + y
  )");

  std::vector<IValue> inputs;
  auto inputx = 5 * torch::ones({});
  auto inputy = 4 * torch::ones({});
  auto ref = m.run_method("add_three", inputx, inputy);

  std::stringstream ss;
  m._save_for_mobile(ss);
  mobile::Module bc = _load_for_mobile(ss);
  IValue res = bc.run_method("add_three", inputx, inputy);

  auto resd = res.toTensor().item<float>();
  auto refd = ref.toTensor().item<float>();
  AT_ASSERT(resd == refd);
}

TEST(LiteInterpreterTest, DuplicateSetState) {
  Module m("M");
  m.register_parameter("foo", torch::ones({}), false);
  m.define(R"(
    def __getstate__(self):
      return self.foo + self.foo
    def __setstate__(self, a):
      self.foo = a
    def forward(self, x):
      b = 4
      return self.foo + x + b
  )");

  Module b("B");
  b.register_module("M0", m);
  b.register_module("M1", m);
  b.define(R"(
    def forward(self, x):
      return self.M0.forward(x) + self.M1.forward(x)
  )");

  std::stringstream ss;
  m._save_for_mobile(ss);
  mobile::Module bc = _load_for_mobile(ss);
  const auto methods = bc.get_methods();
  const size_t expected_n = 3;
  ASSERT_EQ(methods.size(), expected_n);
}

TEST(LiteInterpreterTest, ExtraFiles) {
  const auto script = R"JIT(
    def forward(self):
        x = torch.rand(5, 5)
        x = x.mm(x)
        return x
  )JIT";

  auto module =
      std::make_shared<Module>("Module", std::make_shared<CompilationUnit>());
  module->define(script);
  std::ostringstream oss;
  std::unordered_map<std::string, std::string> extra_files;
  extra_files["metadata.json"] = "abc";
  extra_files["mobile_info.json"] = "{\"key\": 23}";
  module->_save_for_mobile(oss, extra_files);

  std::istringstream iss(oss.str());
  caffe2::serialize::IStreamAdapter adapter{&iss};
  std::unordered_map<std::string, std::string> loaded_extra_files;
  loaded_extra_files["metadata.json"] = "";
  torch::jit::_load_for_mobile(iss, torch::kCPU, loaded_extra_files);
  ASSERT_EQ(loaded_extra_files["metadata.json"], "abc");

  loaded_extra_files.clear();
  std::vector<std::string> all_files =
      caffe2::serialize::PyTorchStreamReader(&iss).getAllRecords();

  for (auto& file_name : all_files) {
    if (file_name.find("extra/") == 0) {
      loaded_extra_files[file_name.substr(6)] = "";
    }
  }

  torch::jit::_load_for_mobile(iss, torch::kCPU, loaded_extra_files);
  ASSERT_EQ(loaded_extra_files["metadata.json"], "abc");
  ASSERT_EQ(loaded_extra_files["mobile_info.json"], "{\"key\": 23}");
}

TEST(LiteInterpreterTest, OpNameExportFetchRootOperators) {
  torch::jit::Module m("m");
  m.register_parameter("weight", torch::ones({20, 1, 5, 5}), false);
  m.register_parameter("bias", torch::ones({20}), false);
  m.define(R"(
    def forward(self, input):
      x1 = torch.zeros(2, 2)
      x2 = torch.empty_like(torch.empty(2, 2))
      x3 = torch._convolution(input, self.weight, self.bias, [1, 1], [0, 0], [1, 1], False, [0, 0], 1, False, False, True, True)
      return (x1, x2, x3)
  )");
  m.eval();

  std::stringstream ss;
  m._save_for_mobile(ss);

  torch::jit::mobile::Module ptl_model = torch::jit::_load_for_mobile(ss);
  std::set<std::string> operator_names =
      torch::jit::mobile::_export_operator_list(ptl_model);
  std::set<std::string> expected_operator_names = {
      "aten::_convolution",
      "aten::empty.memory_format",
      "aten::empty_like",
      "aten::zeros",
  };
  EXPECT_EQ(operator_names, expected_operator_names)
      << "Expected the root operator lists to be the same";
}

TEST(LiteInterpreterTest, DefaultArgsConv) {
  auto s = std::getenv("PYTORCH_TEST_WITH_TSAN");
  if (s && strcmp(s, "1") == 0)
    return;

  std::vector<torch::jit::IValue> inputs;

  Module m("m");
  m.register_parameter("weight", torch::ones({20, 1, 5, 5}), false);
  m.register_parameter("bias", torch::ones({20}), false);
  m.define(R"(
    def forward(self, input):
      return torch.conv2d(input, self.weight, self.bias, [1, 1], [0, 0], [1, 1], 1)
  )");

  inputs.push_back(torch::ones({1, 1, 28, 28}));

  auto outputref = m.forward(inputs).toTensor();

  std::stringstream ss;
  m._save_for_mobile(ss);
  mobile::Module bc = _load_for_mobile(ss);
  IValue res;
  for (int i = 0; i < 1; ++i) {
    res = bc.get_method("forward")(inputs);
  }
  auto output = res.toTensor();
  AT_ASSERT(outputref.dim() == output.dim());
  AT_ASSERT(output.equal(outputref));
}

namespace {
void testLiteModuleCompareResultTensors(
    Module& m,
    const std::vector<torch::jit::IValue>& inputs,
    const std::string& method_name = "forward") {
  auto outputref = m.get_method(method_name)(inputs).toTensor();

  std::stringstream ss;
  m._save_for_mobile(ss);
  mobile::Module bc = _load_for_mobile(ss);
  IValue res;
  for (int i = 0; i < 3; ++i) {
    res = bc.get_method(method_name)(inputs);
  }
  auto output = res.toTensor();
  AT_ASSERT(outputref.dim() == output.dim());
  AT_ASSERT(output.equal(outputref));
}

void testDefaultArgsPinv(int num_args) {
  Module m("m");
  if (num_args == 1) {
    m.define(R"(
      def forward(self, input):
        return torch.linalg_pinv(input)
    )");
  } else if (num_args == 2) {
    m.define(R"(
      def forward(self, input):
        return torch.linalg_pinv(input, 1e-5)
    )");
  } else if (num_args == 3) {
    m.define(R"(
      def forward(self, input):
        return torch.linalg_pinv(input, 1e-5, True)
    )");
  }

  std::vector<torch::jit::IValue> inputs;
  const int N = 28;
  auto input = torch::range(1, N * N, 1);
  input[0] = 1; // a more stable matrix
  input = input.view({N, N});
  inputs.push_back(input);
  testLiteModuleCompareResultTensors(m, inputs);
}
} // namespace

#if !defined FB_XPLAT_BUILD
TEST(LiteInterpreterTest, DefaultArgsPinv) {
  // Test with different number of specified arguments.
  // Arguments not specified take default value.
  for (int num_args = 1; num_args <= 3; ++num_args) {
    testDefaultArgsPinv(num_args);
  }

  //  bytecode with one specified argument:
  //  (6,
  //      ('__torch__.m.forward',
  //          (('instructions',
  //              (('STOREN', 1, 2),
  //                  ('DROPR', 1, 0),
  //                  ('MOVE', 2, 0),
  //                  ('OP', 0, 0),
  //                  ('RET', 0, 0))),
  //              ('operators', (('aten::linalg_pinv', '', 1),)),
  //              ('constants', (False, 1e-15)), # default constants are not
  //              used
  //              ('types', ()),
  //              ('register_size', 2)),
  //          (('arguments',
  //              ((('name', 'self'), ('type', '__torch__.m'), ('default_value',
  //              None)),
  //                  (('name', 'input'), ('type', 'Tensor'), ('default_value',
  //                  None)))),
  //              ('returns',
  //                  ((('name', ''), ('type', 'Tensor'), ('default_value',
  //                  None)),)))))

  //  bytecode with 2 specified argument:
  //  (6,
  //      ('__torch__.m.forward',
  //          (('instructions',
  //              (('STOREN', 1, 2),
  //                  ('DROPR', 1, 0),
  //                  ('MOVE', 2, 0),
  //                  ('LOADC', 1, 0), # added LOADC for specified argument
  //                  ('OP', 0, 0),
  //                  ('RET', 0, 0))),
  //              ('operators', (('aten::linalg_pinv', '', 2),)),
  //              ('constants', (False, 1e-05)), # updated constant table
  //              ('types', ()),
  //              ('register_size', 2)),
  //          (('arguments',
  //              ((('name', 'self'), ('type', '__torch__.m'), ('default_value',
  //              None)),
  //                  (('name', 'input'), ('type', 'Tensor'), ('default_value',
  //                  None)))),
  //              ('returns',
  //                  ((('name', ''), ('type', 'Tensor'), ('default_value',
  //                  None)),)))))

  //  bytecode with 3 specified arguments:
  //  (6,
  //      ('__torch__.m.forward',
  //          (('instructions',
  //              (('STOREN', 1, 2),
  //                  ('DROPR', 1, 0),
  //                  ('MOVE', 2, 0),
  //                  ('LOADC', 1, 0),
  //                  ('LOADC', 0, 0),
  //                  ('OP', 0, 0),
  //                  ('RET', 0, 0))),
  //              ('operators', (('aten::linalg_pinv', '', 3),)),
  //              ('constants', (True, 1e-05)),
  //              ('types', ()),
  //              ('register_size', 2)),
  //          (('arguments',
  //              ((('name', 'self'), ('type', '__torch__.m'), ('default_value',
  //              None)),
  //                  (('name', 'input'), ('type', 'Tensor'), ('default_value',
  //                  None)))),
  //              ('returns',
  //                  ((('name', ''), ('type', 'Tensor'), ('default_value',
  //                  None)),)))))
}

TEST(LiteInterpreterTest, DefaultArgsPinvSpecifyDefault) {
  // The second argument is specified, but the value is the same as the default
  // value. It's treated as "not specified" since the value can be fetched from
  // schema.
  Module m("m");
  m.define(R"(
    def forward(self, input):
      return torch.linalg_pinv(input, 1e-15)
  )");
  torch::jit::MobileCode code(m.get_method("forward").graph(), "forward");
  auto arg_nums = code.op_to_num_specified_args();
  ASSERT_EQ(arg_nums.size(), 1);
  ASSERT_EQ(arg_nums["aten::linalg_pinv"], 1);
  std::vector<torch::jit::IValue> inputs;
  const int N = 28;
  auto input = torch::range(1, N * N, 1);
  input[0] = 1; // a more stable matrix
  input = input.view({N, N});
  inputs.push_back(input);
  testLiteModuleCompareResultTensors(m, inputs);
}

TEST(LiteInterpreterTest, TestExceptionStackWithTwoLevelModuleHierarchy) {
  Module a("A");
  a.define(R"(
    def bar(self, x, y):
      return x + y
  )");
  Module b("B");
  b.register_module("A0", a);
  b.define(R"(
    def foo(self, x, y):
      return self.A0.bar(x, y) + 2
  )");
  Module c("C");
  c.register_module("B0", b);
  c.define(R"(
    def forward(self, x, y):
      return self.B0.foo(x, y) + 3
  )");

  std::vector<IValue> inputs;
  inputs.emplace_back(torch::rand({2, 4}));
  inputs.emplace_back(torch::rand({13, 9}));

  std::stringstream ss;
  c._save_for_mobile(ss, ExtraFilesMap(), true);
  auto lite_m = _load_for_mobile(ss);
  std::string error_pattern = R"(
  Module hierarchy:top(C)::<unknown>.B0(B)::foo.A0(A)::bar.aten::add
Traceback of TorchScript (most recent call last):
  File "<string>", line 3, in <unknown>

    def forward(self, x, y):
      return self.B0.foo(x, y) + 3
             ~~~~~~~~~~~ <--- HERE

  File "<string>", line 3, in foo

    def foo(self, x, y):
      return self.A0.bar(x, y) + 2
             ~~~~~~~~~~~ <--- HERE

  File "<string>", line 3, in bar

    def bar(self, x, y):
      return x + y
             ~~~~~ <--- HERE
  )";
  ASSERT_THROWS_WITH_MESSAGE(lite_m.forward(inputs), error_pattern);
}
#endif // !defined(FB_XPLAT_BUILD)

namespace {
static auto reg =
    torch::class_<TorchBindLiteInterpreterTestStruct>(
        "_TorchScriptTesting",
        "_LiteInterpreterTest")
        .def(torch::init<>())
        .def("get", &TorchBindLiteInterpreterTestStruct::get)
        .def_pickle(
            // __getattr__
            [](const c10::intrusive_ptr<TorchBindLiteInterpreterTestStruct>&
                   self) -> int64_t { return 0; },
            // __setattr__
            [](int64_t state) {
              return c10::make_intrusive<TorchBindLiteInterpreterTestStruct>();
            });

} // namespace

TEST(LiteInterpreterTest, OperatorCacheDifferentiatesDefaultArgs) {
  // Create 3 methods:
  //
  // 1. forward() returns a tensor with dtype=torch.int64 (4)
  // 2. forward2() returns a tensor with dtype=torch.float32 (6)
  // 3. forward3() returns a tensor with dtype=torch.float32 but
  //    the dtype is inferred by the input tensor's dtype
  //
  // If caching works correctly, then the result from the full-jit
  // module and the lite module will be the same. Otherwise, it
  // will be different if we don't correctly ignore the cache
  // entry for an operator that has a different number of
  // arguments.
  Module m("m");
  m.define(R"(
    def forward(self):
      ret1 = torch.new_empty(torch.zeros(10), [10], dtype=4)
      return ret1.fill_(25)
  )");
  m.define(R"(
    def forward2(self):
      ret1 = torch.new_empty(torch.zeros(10), [10], dtype=6)
      return ret1.fill_(32.0)
  )");
  m.define(R"(
    def forward3(self):
      ret1 = torch.new_empty(torch.zeros(10), [10])
      return ret1.fill_(12.0)
  )");

  std::vector<torch::jit::IValue> inputs;
  testLiteModuleCompareResultTensors(m, inputs, "forward");
  testLiteModuleCompareResultTensors(m, inputs, "forward2");
  testLiteModuleCompareResultTensors(m, inputs, "forward3");
}

} // namespace jit
} // namespace torch
