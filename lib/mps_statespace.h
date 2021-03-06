// Copyright 2019 Google LLC. All Rights Reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     https://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#ifndef MPS_STATESPACE_H_
#define MPS_STATESPACE_H_

// For templates will take care of parallelization.
#define EIGEN_DONT_PARALLELIZE 1

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory>

#include "../eigen/Eigen/Dense"

namespace detail {

inline void do_not_free(void*) {}

inline void free(void* ptr) {
#ifdef _WIN32
  _aligned_free(ptr);
#else
  ::free(ptr);
#endif
}

}  // namespace detail

namespace qsim {

namespace mps {
/**
 * Class containing context and routines for fixed bond dimension
 * truncated Matrix Product State (MPS) simulation.
 */
template <typename For, typename fp_type = float>
class MPSStateSpace {
 private:
 public:
  using Pointer = std::unique_ptr<fp_type, decltype(&detail::free)>;

  using Complex = std::complex<fp_type>;
  using Matrix =
      Eigen::Matrix<Complex, Eigen::Dynamic, Eigen::Dynamic, Eigen::RowMajor>;
  using ConstMatrixMap = Eigen::Map<const Matrix>;
  using MatrixMap = Eigen::Map<Matrix>;

  // Store MPS tensors with the following shape:
  // [2, bond_dim], [bond_dim, 2, bond_dim], ... , [bond_dim, 2].
  class MPS {
   public:
    MPS() = delete;

    MPS(Pointer&& ptr, unsigned num_qubits, unsigned bond_dim)
        : ptr_(std::move(ptr)), num_qubits_(num_qubits), bond_dim_(bond_dim) {}

    fp_type* get() { return ptr_.get(); }

    const fp_type* get() const { return ptr_.get(); }

    fp_type* release() {
      num_qubits_ = 0;
      return ptr_.release();
    }

    unsigned num_qubits() const { return num_qubits_; }

    unsigned bond_dim() const { return bond_dim_; }

   private:
    Pointer ptr_;
    unsigned num_qubits_;
    unsigned bond_dim_;
  };

  // Note: ForArgs are currently unused.
  template <typename... ForArgs>
  MPSStateSpace(ForArgs&&... args) : for_(args...) {}

  // Requires num_qubits >= 2 and bond_dim >= 2.
  static MPS CreateMPS(unsigned num_qubits, unsigned bond_dim) {
    auto end_sizes = 2 * 4 * bond_dim;
    auto internal_sizes = 4 * bond_dim * bond_dim * num_qubits;
    // Use two extra "internal style" blocks past the end of the
    //   working allocation for scratch space. Needed for gate
    //   application.
    auto size = sizeof(fp_type) * (end_sizes + internal_sizes);

#ifdef _WIN32
    Pointer ptr{(fp_type*)_aligned_malloc(size, 64), &detail::free};
    bool is_null = ptr.get() != nullptr;
    return MPS{std::move(ptr), is_null ? num_qubits : 0,
               is_null ? bond_dim : 0};
#else
    void* p = nullptr;
    if (posix_memalign(&p, 64, size) == 0) {
      return MPS{Pointer{(fp_type*)p, &detail::free}, num_qubits, bond_dim};
    } else {
      return MPS{Pointer{nullptr, &detail::free}, 0, 0};
    }
#endif
  }

  static unsigned Size(const MPS& state) {
    auto end_sizes = 2 * 4 * state.bond_dim();
    auto internal_sizes = 4 * state.bond_dim() * state.bond_dim();
    return end_sizes + internal_sizes * (state.num_qubits() - 2);
  }

  static unsigned RawSize(const MPS& state) {
    return sizeof(fp_type) * Size(state);
  }

  // Get the pointer offset to the beginning of an MPS block.
  static unsigned GetBlockOffset(const MPS& state, unsigned i) {
    if (i == 0) {
      return 0;
    }
    return 4 * state.bond_dim() * (1 + state.bond_dim() * (i - 1));
  }

  // Copies the state contents of one MPS to another.
  // Ignores scratch data.
  static bool CopyMPS(const MPS& src, MPS& dest) {
    if ((src.num_qubits() != dest.num_qubits()) ||
        src.bond_dim() != dest.bond_dim()) {
      return false;
    }
    auto size = RawSize(src);
    memcpy(dest.get(), src.get(), size);
    return true;
  }

  // Set the MPS to the |0> state.
  static void SetMPSZero(MPS& state) {
    auto size = Size(state);
    memset(state.get(), 0, sizeof(fp_type) * size);
    auto block_size = 4 * state.bond_dim() * state.bond_dim();
    state.get()[0] = 1.0;
    for (unsigned i = 4 * state.bond_dim(); i < size; i += block_size) {
      state.get()[i] = 1.0;
    }
  }

  // Testing only. Convert the MPS to a wavefunction under "normal" ordering.
  // Requires: wf be allocated beforehand with bd * 2 ^ nq -1 memory.
  static void ToWaveFunction(MPS& state, fp_type* wf) {
    const auto bd = state.bond_dim();
    const auto nq = state.num_qubits();
    fp_type* raw_state = state.get();

    ConstMatrixMap accum = ConstMatrixMap((Complex*)(raw_state), 2, bd);
    ConstMatrixMap next_block = ConstMatrixMap(nullptr, 0, 0);
    MatrixMap result2 = MatrixMap(nullptr, 0, 0);
    auto offset = 0;
    auto result2_size = 2;

    for (unsigned i = 1; i < nq - 1; i++) {
      offset = GetBlockOffset(state, i);
      // use of new does not trigger any expensive operations.
      new (&next_block)
          ConstMatrixMap((Complex*)(raw_state + offset), bd, 2 * bd);
      new (&result2) MatrixMap((Complex*)(wf), result2_size, 2 * bd);

      // temp variable used since result2 and accum point to same memory.
      result2 = accum * next_block;
      result2_size *= 2;
      new (&accum) ConstMatrixMap((Complex*)(wf), result2_size, bd);
    }
    offset = GetBlockOffset(state, nq - 1);
    new (&next_block) ConstMatrixMap((Complex*)(raw_state + offset), bd, 2);
    new (&result2) MatrixMap((Complex*)(wf), result2_size, 2);
    result2 = accum * next_block;
  }

 protected:
  For for_;
};

}  // namespace mps
}  // namespace qsim

#endif  // MPS_STATESPACE_H_
