// Taken from https://github.com/hpides/autovec-db

#include <benchmark/benchmark.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cstring>
#include <numeric>
#include <random>
#include <stdexcept>

#include "common.hpp"
#include "simd.hpp"

using RowId = uint32_t;
using DictEntry = uint32_t;

using DictColumn = AlignedData<DictEntry, 64>;
using MatchingRows = AlignedData<RowId, 64>;

// We initially had 1M tuples == 4MB input data, targetting L3 Cache.
// However, on cascadelake, L3 bandwidth was bottlenecking, so we went down to ~125kB, targetting L2.
static constexpr size_t NUM_BASE_ROWS = 16;
static constexpr size_t SCALE_FACTOR = 1024ull * 2;
static constexpr size_t NUM_ROWS = NUM_BASE_ROWS * SCALE_FACTOR;
static constexpr size_t NUM_UNIQUE_VALUES = 16;

struct naive_scan {
  RowId operator()(const DictColumn& column, DictEntry filter_val, MatchingRows* matching_rows) {
    const DictEntry* column_data = column.aligned_data();
    RowId* output = matching_rows->aligned_data();

    RowId num_matching_rows = 0;
    for (RowId row = 0; row < NUM_ROWS; ++row) {
      if (column_data[row] < filter_val) {
        output[num_matching_rows++] = row;
      }
    }
    return num_matching_rows;
  }
};

struct autovec_scan {
  RowId operator()(const DictColumn& column, DictEntry filter_val, MatchingRows* matching_rows) {
    // The naive version should be autovectorizable with clang, but they currently don't do this
    // see https://github.com/llvm/llvm-project/issues/42210
    // According to the issue, ICC can autovectorize this.
    // Godbolt playground: https://godbolt.org/z/aahTPczdr

    const DictEntry* __restrict column_data = column.aligned_data();
    RowId* __restrict output = matching_rows->aligned_data();

    RowId num_matching_rows = 0;
    for (RowId row = 0; row < NUM_ROWS; ++row) {
      output[num_matching_rows] = row;
      num_matching_rows += static_cast<int>(column_data[row] < filter_val);
    }
    return num_matching_rows;
  }
};

enum class Vector512ScanStrategy { SHUFFLE_MASK_16_BIT, SHUFFLE_MASK_8_BIT, SHUFFLE_MASK_4_BIT };

#if AVX512_AVAILABLE
enum class X86512ScanStrategy { COMPRESSSTORE, COMPRESS_PLUS_STORE };

template <X86512ScanStrategy STRATEGY>
struct x86_avx512_512_scan {
  static constexpr uint32_t NUM_MATCHES_PER_VECTOR = sizeof(__m512i) / sizeof(DictEntry);

  RowId operator()(const DictColumn& column, DictEntry filter_val, MatchingRows* matching_rows) {
    const DictEntry* __restrict rows = column.aligned_data();
    RowId* __restrict output = matching_rows->aligned_data();

    const __m512i filter_vec = _mm512_set1_epi32(static_cast<int>(filter_val));
    const __m512i row_id_offsets = _mm512_set_epi32(15, 14, 13, 12, 11, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1, 0);

    RowId num_matching_rows = 0;
    static_assert(NUM_ROWS % NUM_MATCHES_PER_VECTOR == 0);
    for (RowId chunk_start_row = 0; chunk_start_row < NUM_ROWS; chunk_start_row += NUM_MATCHES_PER_VECTOR) {
      // x86: Doing this instead of {start_row + 0, start_row + 1, ...} has a 3x performance improvement! Also applies
      // to the gcc-vec versions.
      const __m512i row_ids = _mm512_add_epi32(_mm512_set1_epi32(static_cast<int>(chunk_start_row)), row_id_offsets);

      const __m512i rows_to_match = _mm512_load_epi32(rows + chunk_start_row);
      const __mmask16 matches = _mm512_cmplt_epi32_mask(rows_to_match, filter_vec);

      if constexpr (STRATEGY == X86512ScanStrategy::COMPRESSSTORE) {
        _mm512_mask_compressstoreu_epi32(output + num_matching_rows, matches, row_ids);
      } else {
        static_assert(STRATEGY == X86512ScanStrategy::COMPRESS_PLUS_STORE);
        auto compressed_rows = _mm512_mask_compress_epi32(row_ids, matches, row_ids);
        _mm512_storeu_epi32(output + num_matching_rows, compressed_rows);
      }

      num_matching_rows += std::popcount(matches);
    }

    return num_matching_rows;
  }
};

#endif

template <typename ScanFn>
void BM_dictionary_scan(benchmark::State& state) {
  DictColumn column{NUM_ROWS};
  MatchingRows matching_rows{NUM_ROWS};

  static_assert(NUM_ROWS % NUM_UNIQUE_VALUES == 0, "Number of rows must be a multiple of num unique values.");
  const int64_t input_percentage = state.range(0);
  const auto percentage_to_pass_filter = static_cast<double>(input_percentage) / 100;

  // Our filter value comparison is `row < filter_value`, so we can control the selectivity as follows:
  //   For percentage =   0, the filter value is                     0, i.e., no values will match.
  //   For percentage =  50, the filter value is NUM_UNIQUE_VALUES / 2, i.e., 50% of all values will match.
  //   For percentage = 100, the filter value is     NUM_UNIQUE_VALUES, i.e., all values will match.
  const auto filter_value = static_cast<DictEntry>(NUM_UNIQUE_VALUES * percentage_to_pass_filter);

  DictEntry* column_data = column.aligned_data();
  for (size_t i = 0; i < NUM_ROWS; ++i) {
    column_data[i] = i % NUM_UNIQUE_VALUES;
  }
  std::mt19937 rng{std::random_device{}()};
  std::shuffle(column_data, column_data + NUM_ROWS, rng);

  // Correctness check with naive implementation
  ScanFn scan_fn{};
  MatchingRows matching_rows_naive{NUM_ROWS};
  const RowId num_matches_naive = naive_scan{}(column, filter_value, &matching_rows_naive);
  const RowId num_matches_specialized = scan_fn(column, filter_value, &matching_rows);

  if (num_matches_naive != num_matches_specialized) {
    throw std::runtime_error{"Bad result. Expected " + std::to_string(num_matches_naive) + " rows to match, but got " +
                             std::to_string(num_matches_specialized)};
  }
  for (size_t i = 0; i < num_matches_naive; ++i) {
    if (matching_rows_naive.aligned_data()[i] != matching_rows.aligned_data()[i]) {
      throw std::runtime_error{"Bad result compare at position: " + std::to_string(i)};
    }
  }

  // Sanity check that the 100 and 0 percent math works out.
  if (input_percentage == 100 && num_matches_specialized != NUM_ROWS) {
    throw std::runtime_error{"Bad result. Did not match all rows."};
  }
  if (input_percentage == 0 && num_matches_specialized != 0) {
    throw std::runtime_error{"Bad result. Did not match 0 rows."};
  }

  benchmark::DoNotOptimize(column.aligned_data());
  benchmark::DoNotOptimize(matching_rows.aligned_data());

  for (auto _ : state) {
    const RowId num_matches = scan_fn(column, filter_value, &matching_rows);
    benchmark::DoNotOptimize(num_matches);
  }

  state.counters["PerValue"] = benchmark::Counter(static_cast<double>(state.iterations() * NUM_ROWS),
                                                  benchmark::Counter::kIsRate | benchmark::Counter::kInvert);
}

// #define BM_ARGS
// Unit(benchmark::kMicrosecond)->Arg(0)->Arg(10)->Arg(33)->Arg(50)->Arg(66)->Arg(100)->ReportAggregatesOnly()
#define BM_ARGS Unit(benchmark::kMicrosecond)->Arg(50)

BENCHMARK(BM_dictionary_scan<naive_scan>)->BM_ARGS;
BENCHMARK(BM_dictionary_scan<autovec_scan>)->BM_ARGS;

#if AVX512_AVAILABLE
// BENCHMARK(BM_dictionary_scan<x86_avx512_512_scan<X86512ScanStrategy::COMPRESSSTORE>>)->BM_ARGS;
BENCHMARK(BM_dictionary_scan<x86_avx512_512_scan<X86512ScanStrategy::COMPRESS_PLUS_STORE>>)->BM_ARGS;
#endif

BENCHMARK_MAIN();
