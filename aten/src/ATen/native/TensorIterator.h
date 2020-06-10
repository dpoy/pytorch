#pragma once

#include <ATen/ATen.h>
#include <c10/util/FunctionRef.h>
#include <c10/util/SmallVector.h>
#include <c10/util/TypeCast.h>
#include <ATen/core/Range.h>
#include <bitset>
#include <ATen/NamedTensorUtils.h>
#include <ATen/Parallel.h>

// TensorIterator is a helper class for element-wise operations, such as
// arithmetic, comparisons, and trigonometric functions. It handles
// broadcasting and type conversions of operands.
//
// This is inspired by NumPy's Array Iterator API (NpyIter).
//
// The files Loops.h and Loops.cuh provide functions to build kernels that
// use TensorIterator.
//
// Example:
//
//   auto iter = TensorIterator();
//   iter.add_output(output);
//   iter.add_input(input);
//   iter.build()
//
// [MyKernel.cpp / MyKernel.cu]
//   cpu_kernel(iter, [](float a, float b) {
//     return a + b;
//   });
//
//   gpu_kernel(iter, []GPU_LAMBDA(float a, float b) -> float {
//     return a + b;
//   });
//
// Note [Common Dtype Computation]
// ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
// Some operations have a natural notion of a "common dtype" or
//   "computation dtype" where all inputs are cast to one dtype, the
//   operation is performed, and then the results are cast to all outputs.
//
// TensorIterator infers a common dtype if all inputs have the same dtype,
//   and it computes one using type promotion rules on its inputs if
//   promote_inputs_to_common_dtype_ is true. Attempting to query
//   a common dtype otherwise will throw an exception.
//
// Note that the outputs are not considered when computing a common dtype.

namespace at {

struct DimCounter {
  DimCounter(IntArrayRef shape, Range range);

  void increment(const std::array<int64_t, 2>& step);
  bool is_done() const;
  std::array<int64_t, 2> max_2d_step() const;

  IntArrayRef shape;
  Range range;
  DimVector values;
  int64_t offset;
};

struct CAFFE2_API OperandInfo {
  using StrideVector = SmallVector<int64_t, 6>;
  OperandInfo() {}
  explicit OperandInfo(const Tensor& t) : tensor(t) {
    if (t.defined()) {
      device = t.device();
      target_dtype = t.scalar_type();
      current_dtype = target_dtype;
    }
    validate();
  }
  OperandInfo(const Tensor& t, Device device, ScalarType dtype)
    : tensor(t), device(device), target_dtype(dtype), current_dtype(t.scalar_type()) {
    validate();
  }

  /// Stride after broadcasting. The stride is in bytes, not number of elements.
  StrideVector stride_bytes;

  /// The tensor operand. Note that the strides, data pointer, and
  /// other attributes may differ due to dimension reordering and
  /// coalescing.
  Tensor tensor;

  // Save the original tensor operand in cases when an output is modified
  // (e.g. if dtype is changed)
  Tensor original_tensor;

  /// The desired device and type for the operand. For inputs, this specifies that
  /// the input should be converted to this type if necessary. For outputs, this
  /// specifies which type to allocate. target_dtype and device are initialized with the dtype and device of the tensor
  /// but during type promotion target_dtype value can become different from tensor's dtype
  /// also, during type promotion target_dtype and device can be set for an undefined tensor so that tensor can be properly
  /// constructed later.
  Device device = kCPU;
  ScalarType target_dtype = ScalarType::Undefined;
  // Caches dtype of the tensor, because scalar_type is an expensive operation
  // If dtype of the tensor is changed (e.g. as a result of type promotion or in allocate_outputs), this
  //value should be changed too.
  ScalarType current_dtype = ScalarType::Undefined;

  bool is_type_defined() const { return target_dtype != ScalarType::Undefined; }
  TensorOptions options() const {
    return TensorOptions(target_dtype).device(device);
  }

  /// The data pointer. This may be different from tensor.data_ptr() if the
  /// iterator is split.
  void* data = nullptr;

  bool is_output = false;

  bool is_read_write = false;

  void validate() {
    TORCH_CHECK(
        !tensor.defined() || tensor.layout() == kStrided,
        "unsupported tensor layout: ", tensor.layout());
  }
};

struct SplitUntil32Bit;

enum class FastSetupType : uint8_t {
  NONE,
  CONTIGUOUS,
  CHANNELS_LAST,
  NON_OVERLAPPING_DENSE
};

struct CAFFE2_API TensorIterator {
  using DimMask = std::bitset<64>;
  using PtrVector = SmallVector<char*, 4>;
  using StrideVector = SmallVector<int64_t, 6>;

  TensorIterator() {}

  // The inner-loop function operates on the fastest moving dimension. It
  // implements element-wise operations in terms of 1-d strided tensors.
  //
  // Arguments:
  //  data: data pointers for each operand (length `ntensors`)
  //  strides: stride for each operand (length `ntensors`)
  //  size: size of inner loop
  //
  // The `size` often matches shape[0], but may be smaller due to
  // parallelization of the inner loop.
  using loop_t = c10::function_ref<void(char** data, const int64_t* strides, int64_t size)>;
  using loop2d_t = c10::function_ref<void(char** data, const int64_t* strides, int64_t size0, int64_t size1)>;

  using loop_subiter_t = c10::function_ref<void(TensorIterator& subiter)>;

  void foreach_reduced_elt(loop_subiter_t loop, bool parallelize=true);

  static TensorIterator binary_op(Tensor& out, const Tensor& a, const Tensor& b,
    bool check_mem_overlap = false);
  static TensorIterator comparison_op(Tensor& out, const Tensor& a, const Tensor& b,
    bool check_mem_overlap = false);
  static TensorIterator unary_op(Tensor& out, const Tensor& a,
    bool check_mem_overlap = false);
  static TensorIterator nullary_op(Tensor& out);
  static TensorIterator reduce_op(Tensor& out, const Tensor& a);
  static TensorIterator reduce_op(Tensor& out1, Tensor& out2, const Tensor& a);

  int ndim() const { return shape_.size(); }
  IntArrayRef shape() const { return shape_; }
  int64_t numel() const;
  int ntensors() const { return operands_.size(); }
  int noutputs() const { return num_outputs_; }
  int ninputs() const { return ntensors() - noutputs(); }
  IntArrayRef view_offsets() const { return view_offsets_; }

  /// number of elements in the output operand. this is the same as numel() for
  /// operations that are not reductions.
  int64_t num_output_elements() const;

  /// number of reduced dimensions in a reduction operation
  int num_reduce_dims() const;

  /// 1-dimensional iteration and no buffering or type conversion
  bool is_trivial_1d() const;
  /// Reducible to 1-dimensional and all operands are contiguous
  bool is_contiguous() const;
  bool is_dim_reduced(int dim) const;

  /// Accessors for each operand
  IntArrayRef strides(int arg) const { return operands_[arg].stride_bytes; }
  void* data_ptr(int arg) const;
  ScalarType dtype(int arg=0) const { return operands_[arg].current_dtype; }
  ScalarType common_dtype() const {
    TORCH_INTERNAL_ASSERT(common_dtype_ != ScalarType::Undefined, "Queried for invalid common dtype!");
    return common_dtype_;
  }
  ScalarType input_dtype(int arg=0) const { return operands_[num_outputs_ + arg].current_dtype; }
  Device device(int arg=0) const { return operands_[arg].device; }
  DeviceType device_type(int arg=0) const { return device(arg).type(); }
  int64_t element_size(int arg) const { return elementSize(dtype(arg)); }
  bool is_scalar(int arg) const;
  bool is_cpu_scalar(int arg) const;

  const Tensor& tensor(int arg) const { return operands_[arg].tensor; }
  Tensor& tensor(int arg) { return operands_[arg].tensor; }

  Tensor output(int arg=0) const {
    AT_ASSERT(arg < num_outputs_);
    return operands_[arg].tensor;
  }

  // Copies from temporary outputs back to the original outputs
  // NOTE: only used on CPU
  void cast_outputs();

  Tensor input(int arg=0) const {
    AT_ASSERT(arg >= 0 && arg < ntensors() - num_outputs_);
    return operands_[num_outputs_ + arg].tensor;
  }

  /// Removes an operand from this iterator
  void remove_operand(int arg);
  /// Removes a dimension from this iterator
  void remove_dimension(int dim);
  /// Shrinks an iterated dimension
  void narrow(int dim, int64_t start, int64_t size);
  /// Narrows every dim after and including `start_dim` to size one.
  void select_all_keeping_dim(int start_dim, IntArrayRef starts);
  /// Replaces the data pointer for the operand at index `arg`.
  /// The new pointer should have the same sizes, strides and dtype as the
  /// original
  void unsafe_replace_operand(int arg, void* data);

  /// Splits this TensorIterator into two iterators. Together they iterate over
  /// the entire operation. Used by `with_32bit_indexing()`.
  std::unique_ptr<TensorIterator> split(int dim);

  /// Returns the dimension with the largest extent: (size[dim]-1) * stride[dim]
  int get_dim_to_split() const;

  template <typename T>
  T scalar_value(int arg) {
    auto& op = operands_[arg];
    return c10::fetch_and_cast<T>(op.tensor.scalar_type(), op.data);
  }

  void for_each(loop_t loop, int64_t grain_size = at::internal::GRAIN_SIZE);
  void for_each(loop2d_t loop, int64_t grain_size = at::internal::GRAIN_SIZE);

  void parallel_reduce(loop2d_t loop);

  void serial_for_each(loop_t loop, Range range) const;
  void serial_for_each(loop2d_t loop, Range range) const;

  /// Create a strides array for a Tensor with shape of this iterator. The
  /// parameter `element_size` specifies the size of Tensor's data type in
  /// bytes (e.g. `4` for `float`)
  StrideVector compatible_stride(int element_size) const;

  /// Inverts the re-ordering done by reorder_dimensions. This can only be
  /// called *before* coalesce_dimensions() is called.
  DimVector invert_perm(IntArrayRef input) const;

  /// Reapply same re-ordering as it is done by reorder_dimensions. This can
  /// only be called *before* coalesce_dimensions() is called.
  DimVector apply_perm_and_mul(IntArrayRef input, int mul) const;

  /// Helper functions for CPU iteration
  StrideVector get_dim_strides(int dim) const;
  StrideVector get_strides() const;
  StrideVector get_inner_strides() const { return get_dim_strides(0); }
  PtrVector get_data_ptrs(ArrayRef<char*> base, IntArrayRef counter) const;
  PtrVector get_base_ptrs() const;

  /// true if the stride computation can use 32-bit arithmetic. Used by GPU kernels
  bool can_use_32bit_indexing() const;

  /// An "iteratable" object that recursively splits this iterator into sub-iterators
  /// that can use 32-bit indexing.
  SplitUntil32Bit with_32bit_indexing() const;

  /// If the kernel should accumulate into the output. Only relevant for CUDA
  /// reductions.
  bool should_accumulate() const { return accumulate_; }

  /// Whether this iterator produces the actual output,
  /// as opposed to something that will be accumulated further. Only relevant for
  /// CUDA reductions.
  bool is_final_output() const { return final_output_; }

  bool has_contiguous_first_dim() const {
    int num_tensors = ntensors();
    for (int i = 0; i < num_tensors; i++) {
      if (strides(i)[0] != element_size(i)) {
        return false;
      }
    }
    return true;
  }

  void set_check_mem_overlap(bool check_mem_overlap) {
    check_mem_overlap_ = check_mem_overlap;
  }

  /// Construction
  void add_output(const Tensor& output) {
    operands_.emplace_back(output);
    num_outputs_++;
  }

  void add_output(const Tensor& input, Device device, ScalarType dtype) {
    operands_.emplace_back(input, device, dtype);
    num_outputs_++;
  }

  void add_input(const Tensor& input) {
    operands_.emplace_back(input);
  }

  void add_input(const Tensor& input, Device device, ScalarType dtype) {
    operands_.emplace_back(input, device, dtype);
  }

  // Sets the check_all_same_dtype_ flag, which is true by default
  // If true, checks that all inputs and defined outputs have the same dtype
  // Setting either of promote_inputs_to_common_dtype_
  //   or cast_common_dtype_to_outputs_ to true will set
  //   check_all_same_dtype_ to false.
  void check_all_same_dtype(const bool _check_all_same_dtype) {
    check_all_same_dtype_ = _check_all_same_dtype;
  }

  // Sets the check_all_same_device_ flag, which is true by default
  // If true, all operands must be on the same device, with the possible
  //   exception of CPU scalars, which can be passed to some CUDA kernels
  //   as kernel arguments.
  void check_all_same_device(const bool _check_all_same_device) {
    check_all_same_device_ = _check_all_same_device;
  }

  // Sets the enforce_safe_casting_to_output_ flag, which is false by default
  // If true, the iterator's "common dtype" must be computable
  //   (see the [Common Dtype Computation] note) and
  //   canCast(common dtype, output dtype) must be true for all outputs.
  void enforce_safe_casting_to_output(const bool _enforce_safe_casting_to_output) {
    enforce_safe_casting_to_output_ = _enforce_safe_casting_to_output;
  }

  // Sets the promote_inputs_to_common_dtype_ flag, which is false by default
  // If true, the iterator's "common dtype" is always computed (see the
  //   [Common Dtype Computation] note) and, on the CPU, temporary copies of
  //   the inputs in the common dtype are passed as the actual inputs to
  //   the operation.
  // Setting this flag to true sets check_all_same_dtype_ to false.
  void promote_inputs_to_common_dtype(const bool _promote_inputs_to_common_dtype) {
    promote_inputs_to_common_dtype_ = _promote_inputs_to_common_dtype;
    if (_promote_inputs_to_common_dtype) {
      check_all_same_dtype_ = false;
    }
  }

  // Sets the cast_common_dtype_to_outputs_ flag, which is false by default
  // If true, the iterator's "common dtype" must be computatable
  //   (see the [Common Dtype Computation] note) and, on the CPU, temporary
  //   copies of the outputs are passed as the actual output to the operation.
  //   These temporaries are then copied to the original outputs after
  //   the operation is performed (see cast_outputs()).
  // Setting this flag to true sets check_all_same_dtype_ to false.
  void cast_common_dtype_to_outputs(const bool _cast_common_dtype_to_outputs) {
    cast_common_dtype_to_outputs_ = _cast_common_dtype_to_outputs;
    if (_cast_common_dtype_to_outputs) {
      check_all_same_dtype_ = false;
    }
  }

  void dont_resize_outputs() {
    resize_outputs_ = false;
  }

  void declare_static_shape(IntArrayRef shape) {
    // WARNING:
    //   This will bypass all shape checking in the TensorIterator. Kernels which call this method
    //   are expected to check shapes before calling `add_input` or `add_output`.
    TORCH_CHECK(!resize_outputs_, "dont_resize_outputs() must be called before declare_static_shape(...)")
    shape_ = shape;
    static_shape_ = true;
  }

  void declare_static_shape(IntArrayRef shape, const int64_t squash_dim){
    declare_static_shape(shape);
    if (!shape_.size()) return;
    TORCH_CHECK(squash_dim >= 0 && squash_dim < static_cast<int64_t>(shape_.size()),
                "squash_dim ", squash_dim, " must be in [0, ", shape_.size(), ").");
    shape_[squash_dim] = 1;
  }

  void build();

protected:
  void mark_outputs();
  void check_mem_overlaps();
  void compute_shape();
  void compute_strides();
  void reorder_dimensions();
  void permute_dimensions(IntArrayRef perm);
  void compute_types();
  ScalarType compute_common_dtype();
  void allocate_outputs();
  bool fast_set_up();
  FastSetupType compute_fast_setup_type();
  void compute_names();
  void propagate_names_to_outputs();
  void coalesce_dimensions();
  void analyze_memory_format();

protected:
  DimVector shape_;
  DimVector perm_;
  /// The index offsets into the original tensors for each dimension
  DimVector view_offsets_;
  NameVector names_;
  SmallVector<OperandInfo, 4> operands_;
  int num_outputs_ = 0;
  ScalarType common_dtype_ = ScalarType::Undefined;
  bool has_coalesced_dimensions_ = false;
  bool accumulate_ = false;
  bool resize_outputs_ = true;
  bool is_reduction_ = false;
  bool allow_cpu_scalars_ = false;
  bool final_output_ = true;
  bool check_mem_overlap_ = false;
  bool all_ops_same_shape_ = false;
  bool requires_channels_last_output_ = false;
  bool requires_channels_last_3d_output_ = false;
  bool static_shape_ = false;
  bool check_all_same_dtype_ = true;
  bool check_all_same_device_ = true;
  bool enforce_safe_casting_to_output_ = false;
  bool promote_inputs_to_common_dtype_ = false;
  bool cast_common_dtype_to_outputs_ = false;
};
/// A container-like struct that acts as if it contains splits of a
/// TensorIterator that can use 32-bit indexing. Taken together the splits cover
/// the original TensorIterator.
struct CAFFE2_API SplitUntil32Bit {
  struct CAFFE2_API iterator {
    iterator() {};
    iterator(const TensorIterator& iter);
    iterator(iterator&&) = default;

    TensorIterator& operator*() const;
    iterator& operator++();
    bool operator==(const iterator& other) const {
      // two iterators are equal if they are the same object or they're both empty
      return this == &other || (vec.empty() && other.vec.empty());
    }
    // needed for C++11 range-based for loop
    bool operator!=(const iterator& other) const { return !(*this == other); }

    /// stack of TensorIterators to be split
    std::vector<std::unique_ptr<TensorIterator>> vec;
  };

  SplitUntil32Bit(const TensorIterator& iter) : iter(iter) {}

  iterator begin() const;
  iterator end() const;

private:
  const TensorIterator& iter;
};

}  // namespace at
