/* Copyright 2019 The TensorFlow Authors. All Rights Reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
==============================================================================*/

#ifdef INTEL_MKL

#include "tensorflow/core/framework/fake_input.h"
#include "tensorflow/core/framework/node_def_builder.h"
#include "tensorflow/core/framework/tensor.h"
#include "tensorflow/core/kernels/ops_testutil.h"
#include "tensorflow/core/kernels/ops_util.h"
#include "tensorflow/core/lib/core/status_test_util.h"
#include "tensorflow/core/platform/test.h"
#include "tensorflow/core/platform/test_benchmark.h"
#include "tensorflow/core/util/mkl_util.h"

namespace tensorflow {

class MklDequantizeOpTest : public OpsTestBase {};

static const uint8 dummy_tensor[] = {0, 0, 0, 0, 0, 0, 0, 0};
static const TensorShape dummy_shape({8});

TEST_F(MklDequantizeOpTest, small) {
  NodeDefBuilder builder =
      NodeDefBuilder("dequantize_op", NativeFormatEnabled()
                                          ? "_MklNativeDequantize"
                                          : "_MklDequantize")
          .Input(FakeInput(DT_QUINT8))
          .Input(FakeInput(DT_FLOAT))
          .Input(FakeInput(DT_FLOAT))
          .Attr("T", DataTypeToEnum<quint8>::v())
          .Attr("mode", "SCALED")
          .Attr("_kernel", "QuantizedMklOp");
  if (!NativeFormatEnabled()) {
    // Add MKL metadata tensors
    builder.Input(FakeInput(DT_UINT8))
        .Input(FakeInput(DT_UINT8))
        .Input(FakeInput(DT_UINT8));
  }
  TF_ASSERT_OK(builder.Finalize(node_def()));
  TF_ASSERT_OK(InitOp());
  AddInputFromArray<quint8>(TensorShape({1, 2, 2, 2}),
                            {0, 10, 50, 40, 25, 115, 190, 255});
  // min_range = 0
  AddInputFromArray<float>(TensorShape({1}), {0});
  // max_range = 200
  AddInputFromArray<float>(TensorShape({1}), {200.0f});
  if (!NativeFormatEnabled()) {
    AddInputFromArray<uint8>(dummy_shape, dummy_tensor);
    AddInputFromArray<uint8>(dummy_shape, dummy_tensor);
    AddInputFromArray<uint8>(dummy_shape, dummy_tensor);
  }
  TF_ASSERT_OK(RunOpKernel());
  Tensor expected(allocator(), DT_FLOAT, TensorShape({1, 2, 2, 2}));
  test::FillValues<float>(&expected,
                          {0.0, 7.84, 39.21, 31.37, 19.6, 90.2, 149.0, 200});
  const Tensor& output = *GetOutput(0);
  test::ExpectTensorNear<float>(expected, output, 0.1);
}

Tensor CreateMklInput() {
  MklDnnShape mkl_shape;
  memory::desc md =
      memory::desc({1, 2, 2, 2}, MklDnnType<uint8>(), memory::format_tag::nhwc);
  mkl_shape.SetMklTensor(true);
  mkl_shape.SetMklLayout(&md);
  mkl_shape.SetElemType(MklDnnType<uint8>());
  mkl_shape.SetTfLayout(4, {1, 2, 2, 2}, MKL_TENSOR_FORMAT_NHWC);

  DataType dtype = DataTypeToEnum<uint8>::v();
  Tensor mkl_tensor(dtype, {mkl_shape.GetSerializeBufferSize()});
  mkl_shape.SerializeMklDnnShape(
      mkl_tensor.flat<uint8>().data(),
      mkl_tensor.flat<uint8>().size() * sizeof(uint8));
  return mkl_tensor;
}

template <typename T>
class CommonTestUtilities : public OpsTestBase {
 public:
  void MklToTF(const Tensor& tensor, const Tensor& mkl_meta_tensor,
               Tensor* output) {
    // Create an MKL to TF conversion node and execute it
    TF_ASSERT_OK(NodeDefBuilder("mkl_to_tf_op", "_MklToTf")
                     .Input(FakeInput(DataTypeToEnum<T>::v()))
                     .Input(FakeInput(DT_UINT8))  // MKL second tensor
                     .Attr("T", DataTypeToEnum<T>::v())
                     .Attr("_kernel", "MklLayoutDependentOp")
                     .Finalize(node_def()));
    TF_ASSERT_OK(InitOp());
    AddInputFromArray<T>(tensor.shape(), tensor.flat<T>());
    AddInputFromArray<uint8>(mkl_meta_tensor.shape(),
                             mkl_meta_tensor.flat<uint8>());
    TF_ASSERT_OK(RunOpKernel());

    *output = *GetOutput(0);
  }

  void ConvertAndCompare(const Tensor& tensor, const Tensor& mkl_meta_tensor,
                         const Tensor& expected) {
    Tensor output;
    MklToTF(tensor, mkl_meta_tensor, &output);
    test::ExpectTensorNear<T>(expected, output, 0.1);
  }

  void TestBody() {}
};

TEST_F(MklDequantizeOpTest, MKLInput) {
  TF_ASSERT_OK(NodeDefBuilder("dequantize_op", "_MklDequantize")
                   .Input(FakeInput(DT_QUINT8))
                   .Input(FakeInput(DT_FLOAT))
                   .Input(FakeInput(DT_FLOAT))
                   .Input(FakeInput(DT_UINT8))  // MKL second tensor
                   .Input(FakeInput(DT_UINT8))  // MKL second tensor
                   .Input(FakeInput(DT_UINT8))  // MKL second tensor
                   .Attr("T", DataTypeToEnum<quint8>::v())
                   .Attr("mode", "SCALED")
                   .Attr("_kernel", "QuantizedMklOp")
                   .Finalize(node_def()));
  TF_ASSERT_OK(InitOp());
  AddInputFromArray<quint8>(TensorShape({1, 2, 2, 2}),
                            {0, 10, 50, 40, 25, 115, 190, 255});
  // min_range = 0
  AddInputFromArray<float>(TensorShape({1}), {0});
  // max_range = 200
  AddInputFromArray<float>(TensorShape({1}), {200.0f});
  auto mkl_tensor = CreateMklInput();
  AddInputFromArray<uint8>(mkl_tensor.shape(), mkl_tensor.flat<uint8>());
  AddInputFromArray<uint8>(dummy_shape, dummy_tensor);
  AddInputFromArray<uint8>(dummy_shape, dummy_tensor);
  TF_ASSERT_OK(RunOpKernel());
  Tensor expected(allocator(), DT_FLOAT, TensorShape({1, 2, 2, 2}));
  test::FillValues<float>(&expected,
                          {0.0, 7.84, 39.21, 31.37, 19.6, 90.2, 149.0, 200});
  CommonTestUtilities<float> test_util;
  test_util.ConvertAndCompare(*GetOutput(0), *GetOutput(1), expected);
}

}  // namespace tensorflow

#endif  // INTEL_MKL
