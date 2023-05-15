/**
 *  @file usearch.hpp
 *  @author Ashot Vardanian
 *  @brief Single-header Vector Search.
 *  @date 2023-04-26
 *
 *  @copyright Copyright (c) 2023
 */
#ifndef UNUM_USEARCH_H
#define UNUM_USEARCH_H

#if defined(WIN32) || defined(_WIN32) || defined(__WIN32__) || defined(__NT__)
#define _USE_MATH_DEFINES

#include <Windows.h>

#define usearch_pack_m
#define usearch_align_m __declspec(align(64))
#define WINDOWS

#else
#include <fcntl.h>    // `fallocate`
#include <stdlib.h>   // `posix_memalign`
#include <sys/mman.h> // `mmap`
#include <unistd.h>   // `open`, `close`

#define usearch_pack_m __attribute__((packed))
#define usearch_align_m __attribute__((aligned(64)))
#endif

#include <sys/stat.h> // `fstat` for file size

#include <algorithm> // `std::sort_heap`
#include <atomic>    // `std::atomic`
#include <bitset>    // `std::bitset`
#include <climits>   // `CHAR_BIT`
#include <cmath>     // `std::sqrt`
#include <cstring>   // `std::memset`
#include <mutex>     // `std::unique_lock` - replacement candidate
#include <random>    // `std::default_random_engine` - replacement candidate
#include <stdexcept> // `std::runtime_exception`
#include <utility>   // `std::exchange`
#include <vector>    // `std::vector`

#if defined(__GNUC__)
// https://gcc.gnu.org/onlinedocs/gcc/Other-Builtins.html
// Zero means we are only going to read from that memory.
// Three means high temporal locality and suggests to keep
// the data in all layers of cache.
#define prefetch_m(ptr) __builtin_prefetch((void*)(ptr), 0, 3)
#elif defined(__x86_64__)
#define prefetch_m(ptr) _mm_prefetch((void*)(ptr), _MM_HINT_T0)
#else
#define prefetch_m(ptr)
#endif

#if defined(NDEBUG)
#define assert_m(must_be_true, message)
#else
#define assert_m(must_be_true, message)                                                                                \
    if (!(must_be_true)) {                                                                                             \
        throw std::runtime_error(message);                                                                             \
    }
#endif

namespace unum {
namespace usearch {

using f64_t = double;
using f32_t = float;

template <typename at> at angle_to_radians(at angle) noexcept { return angle * M_PI / 180.f; }

template <typename at> at square(at value) noexcept { return value * value; }

inline std::size_t ceil2(std::size_t v) noexcept {
    v--;
    v |= v >> 1;
    v |= v >> 2;
    v |= v >> 4;
    v |= v >> 8;
    v |= v >> 16;
    v |= v >> 32;
    v++;
    return v;
}

template <typename scalar_at, typename result_at = scalar_at> struct ip_gt {
    using scalar_t = scalar_at;
    using result_t = result_at;
    using result_type = result_t;

    inline result_t operator()(scalar_t const* a, scalar_t const* b, std::size_t dim, std::size_t = 0) const noexcept {
        result_type ab{};
#if defined(__GNUC__)
#pragma GCC ivdep
#elif defined(__clang__)
#pragma clang loop vectorize(enable)
#elif defined(_OPENMP)
#pragma omp simd reduction(+ : ab)
#endif
        for (std::size_t i = 0; i != dim; ++i)
            ab += result_t(a[i]) * result_t(b[i]);
        return 1 - ab;
    }
};

template <typename scalar_at, typename result_at = scalar_at> struct cos_gt {
    using scalar_t = scalar_at;
    using result_t = result_at;
    using result_type = result_t;

    inline result_t operator()(scalar_t const* a, scalar_t const* b, std::size_t dim, std::size_t = 0) const noexcept {
        result_t ab{}, a2{}, b2{};
#if defined(__GNUC__)
#pragma GCC ivdep
#elif defined(__clang__)
#pragma clang loop vectorize(enable)
#elif defined(_OPENMP)
#pragma omp simd reduction(+ : ab, a2, b2)
#endif
        for (std::size_t i = 0; i != dim; ++i)
            ab += result_t(a[i]) * result_t(b[i]), //
                a2 += square<result_t>(a[i]),      //
                b2 += square<result_t>(b[i]);
        return ab / (std::sqrt(a2) * std::sqrt(b2));
    }
};

template <typename scalar_at, typename result_at = scalar_at> struct l2sq_gt {
    using scalar_t = scalar_at;
    using result_t = result_at;
    using result_type = result_t;

    inline result_t operator()(scalar_t const* a, scalar_t const* b, std::size_t dim, std::size_t = 0) const noexcept {
        result_t ab_deltas_sq{};
#if defined(__GNUC__)
#pragma GCC ivdep
#elif defined(__clang__)
#pragma clang loop vectorize(enable)
#elif defined(_OPENMP)
#pragma omp simd reduction(+ : ab_deltas_sq)
#endif
        for (std::size_t i = 0; i != dim; ++i)
            ab_deltas_sq += square(result_t(a[i]) - result_t(b[i]));
        return ab_deltas_sq;
    }
};

/**
 *  @brief  Hamming distance computes the number of differing elements in
 *          two arrays. An example would be a chess board.
 */
template <typename scalar_at, typename result_at = std::size_t> struct hamming_gt {
    using scalar_t = scalar_at;
    using result_t = result_at;
    using result_type = result_t;

    inline result_t operator()(scalar_t const* a, scalar_t const* b, std::size_t elements,
                               std::size_t = 0) const noexcept {
        result_t matches{};
#if defined(__GNUC__)
#pragma GCC ivdep
#elif defined(__clang__)
#pragma clang loop vectorize(enable)
#elif defined(_OPENMP)
#pragma omp simd reduction(+ : matches)
#endif
        for (std::size_t i = 0; i != elements; ++i)
            matches += a[i] != b[i];
        return matches;
    }
};

/**
 *  @brief  Hamming distance computes the number of differing bits in
 *          two arrays of integers. An example would be a textual document,
 *          tokenized and hashed into a fixed-capacity bitset.
 */
template <typename scalar_at, typename result_at = std::size_t> struct bit_hamming_gt {
    using scalar_t = scalar_at;
    using result_t = result_at;
    using result_type = result_t;
    static_assert(std::is_unsigned<scalar_t>::value, "Hamming distance requires unsigned integral words");
    static constexpr std::size_t bits_per_word_k = sizeof(scalar_t) * CHAR_BIT;

    inline result_t operator()(scalar_t const* a, scalar_t const* b, std::size_t words,
                               std::size_t = 0) const noexcept {
        result_t matches{};
#if defined(__GNUC__)
#pragma GCC ivdep
#elif defined(__clang__)
#pragma clang loop vectorize(enable)
#elif defined(_OPENMP)
#pragma omp simd reduction(+ : matches)
#endif
        for (std::size_t i = 0; i != words; ++i)
            matches += std::bitset<bits_per_word_k>(a[i] ^ b[i]).count();
        return matches;
    }
};

/**
 *  @brief  Counts the number of matching elements in two unique sorted sets.
 *          Can be used to compute the similarity between two textual documents
 *          using the IDs of tokens present in them.
 */
template <typename scalar_at, typename result_at = scalar_at> struct jaccard_gt {
    using scalar_t = scalar_at;
    using result_t = result_at;
    using result_type = result_t;
    static_assert(!std::is_floating_point<scalar_t>::value, "Jaccard distance requires integral scalars");

    inline result_t operator()( //
        scalar_t const* a, scalar_t const* b, std::size_t a_length, std::size_t b_length) const noexcept {
        result_t intersection{};
        std::size_t i{};
        std::size_t j{};
        while (i != a_length && j != b_length) {
            intersection += a[i] == b[j];
            i += a[i] < b[j];
            j += a[i] >= b[j];
        }
        return 1.f - intersection * 1.f / (a_length + b_length - intersection);
    }
};

/**
 *  @brief  Counts the number of matching elements in two unique sorted sets.
 *          Can be used to compute the similarity between two textual documents
 *          using the IDs of tokens present in them.
 */
template <typename scalar_at, typename result_at = scalar_at> struct pearson_correlation_gt {
    using scalar_t = scalar_at;
    using result_t = result_at;
    using result_type = result_t;

    inline result_t operator()( //
        scalar_t const* a, scalar_t const* b, std::size_t dim, std::size_t = 0) const noexcept {
        result_t a_sum{}, b_sum{}, ab_sum{};
        result_t a_sq_sum{}, b_sq_sum{};
#if defined(__GNUC__)
#pragma GCC ivdep
#elif defined(__clang__)
#pragma clang loop vectorize(enable)
#elif defined(_OPENMP)
#pragma omp simd reduction(+ : a_sum, b_sum, ab_sum, a_sq_sum, b_sq_sum)
#endif
        for (std::size_t i = 0; i != dim; ++i) {
            a_sum += a[i];
            b_sum += b[i];
            ab_sum += a[i] * b[i];
            a_sq_sum += a[i] * a[i];
            b_sq_sum += b[i] * b[i];
        }
        result_t denom = std::sqrt((dim * a_sq_sum - a_sum * a_sum) * (dim * b_sq_sum - b_sum * b_sum));
        result_t corr = (dim * ab_sum - a_sum * b_sum) / denom;
        return -corr;
    }
};

/**
 *  @brief  Haversine distance for the shortest distance between two nodes on
 *          the surface of a 3D sphere, defined with latitude and longitude.
 */
template <typename scalar_at, typename result_at = scalar_at> struct haversine_gt {
    using scalar_t = scalar_at;
    using result_t = result_at;
    using result_type = result_t;
    static_assert(!std::is_integral<scalar_t>::value, "Latitude and longitude must be floating-node");

    inline result_t operator()(scalar_t const* a, scalar_t const* b, std::size_t = 2, std::size_t = 2) const noexcept {
        result_t lat_a = a[0], lon_a = a[1];
        result_t lat_b = b[0], lon_b = b[1];

        result_t lat_delta = angle_to_radians<result_t>(lat_b - lat_a);
        result_t lon_delta = angle_to_radians<result_t>(lon_b - lon_a);

        result_t converted_lat_a = angle_to_radians<result_t>(lat_a);
        result_t converted_lat_b = angle_to_radians<result_t>(lat_b);

        result_t x = //
            square(std::sin(lat_delta / 2.f)) +
            std::cos(converted_lat_a) * std::cos(converted_lat_b) * square(std::sin(lon_delta / 2.f));

        return std::atan2(std::sqrt(x), std::sqrt(1.f - x));
    }
};

template <std::size_t multiple_ak> inline std::size_t divide_round_up(std::size_t num) noexcept {
    return (num + multiple_ak - 1) / multiple_ak;
}

template <typename allocator_at = std::allocator<char>> class visits_bitset_gt {
    using allocator_t = allocator_at;
    using byte_t = typename allocator_t::value_type;
    static_assert(sizeof(byte_t) == 1, "Allocator must allocate separate addressable bytes");

    using slot_t = std::uint64_t;

    std::uint64_t* u64s_{};
    std::size_t slots_{};

  public:
    visits_bitset_gt() noexcept {}
    visits_bitset_gt(std::size_t capacity) noexcept {
        slots_ = divide_round_up<64>(capacity);
        u64s_ = (slot_t*)allocator_t{}.allocate(slots_ * sizeof(slot_t));
    }
    ~visits_bitset_gt() noexcept { allocator_t{}.deallocate((byte_t*)u64s_, slots_ * sizeof(slot_t)), u64s_ = nullptr; }
    void clear() noexcept { std::memset(u64s_, 0, slots_ * sizeof(slot_t)); }
    inline bool test(std::size_t i) const noexcept { return u64s_[i / 64] & (1ul << (i & 63ul)); }
    inline void set(std::size_t i) noexcept { u64s_[i / 64] |= (1ul << (i & 63ul)); }

    visits_bitset_gt(visits_bitset_gt&& other) noexcept {
        u64s_ = other.u64s_;
        slots_ = other.slots_;
        other.u64s_ = nullptr;
        other.slots_ = 0;
    }

    visits_bitset_gt& operator=(visits_bitset_gt&& other) noexcept {
        std::swap(u64s_, other.u64s_);
        std::swap(slots_, other.slots_);
        return *this;
    }
};

using visits_bitset_t = visits_bitset_gt<>;

/**
 *  @brief  Similar to `std::priority_queue`, but allows raw access to underlying
 *          memory, in case you want to shuffle it or sort.
 */
template <typename element_at,                                //
          typename comparator_at = std::less<void>,           // <void> is needed before C++14.
          typename allocator_at = std::allocator<element_at>> //
class max_heap_gt {
  public:
    using element_t = element_at;
    using comparator_t = comparator_at;
    using allocator_t = allocator_at;

    using value_type = element_t;

  private:
    element_t* elements_;
    std::size_t size_;
    std::size_t capacity_;
    std::size_t max_capacity_;

  public:
    max_heap_gt(std::size_t max_capacity = 0) noexcept
        : elements_(nullptr), size_(0), capacity_(0), max_capacity_(max_capacity) {}

    max_heap_gt(max_heap_gt&& other) noexcept {
        std::swap(elements_, other.elements_);
        std::swap(size_, other.size_);
        std::swap(capacity_, other.capacity_);
        std::swap(max_capacity_, other.max_capacity_);
    }
    max_heap_gt& operator=(max_heap_gt&& other) noexcept {
        std::swap(elements_, other.elements_);
        std::swap(size_, other.size_);
        std::swap(capacity_, other.capacity_);
        std::swap(max_capacity_, other.max_capacity_);
        return *this;
    }

    max_heap_gt(max_heap_gt const&) = delete;
    max_heap_gt& operator=(max_heap_gt const&) = delete;

    bool empty() const noexcept { return !size_; }
    std::size_t size() const noexcept { return size_; }
    std::size_t capacity() const noexcept { return capacity_; }
    /// @brief  Selects the largest element in the heap.
    /// @return Reference to the stored element.
    element_t const& top() const noexcept { return elements_[0]; }

    void clear() noexcept {
        while (size_) {
            size_--;
            elements_[size_].~element_t();
        }
    }

    bool reserve(std::size_t n) noexcept {
        if (n < capacity_)
            return true;
        if (max_capacity_ && capacity_ == max_capacity_)
            return false;

        auto new_capacity = (std::max<std::size_t>)(capacity_ * 2u, 16u);
        if (max_capacity_)
            new_capacity = (std::min)(new_capacity, max_capacity_);

        auto allocator = allocator_t{};
        auto new_elements = allocator.allocate(new_capacity);
        if (!new_elements)
            return false;

        if (elements_) {
            std::uninitialized_copy_n(elements_, size_, new_elements);
            allocator.deallocate(elements_, capacity_);
        }
        elements_ = new_elements;
        capacity_ = new_capacity;
        return new_elements;
    }

    template <typename... args_at> //
    bool emplace(args_at&&... args) noexcept {
        if (!reserve(size_ + 1))
            return false;

        new (&elements_[size_]) element_t({std::forward<args_at>(args)...});
        size_++;
        shift_up(size_ - 1);
        return true;
    }

    element_t pop() noexcept {
        element_t result = top();
        std::swap(elements_[0], elements_[size_ - 1]);
        size_--;
        elements_[size_].~element_t();
        shift_down(0);
        return result;
    }

    /** @brief Invalidates the "max-heap" property, transforming into ascending range. */
    void sort_ascending() noexcept { std::sort_heap(elements_, elements_ + size_, &less); }
    /** @brief Invalidates the "max-heap" property, transforming into descending range. */
    void sort_descending() noexcept { sort_ascending(), std::reverse(elements_, elements_ + size_); }
    void sort_heap() noexcept { std::make_heap(elements_, elements_ + size_, &less); }
    void shrink(std::size_t n) noexcept { size_ = n; }

    element_t* data() noexcept { return elements_; }
    element_t const* data() const noexcept { return elements_; }

  private:
    inline std::size_t parent_idx(std::size_t i) const noexcept { return (i - 1u) / 2u; }
    inline std::size_t left_child_idx(std::size_t i) const noexcept { return (i * 2u) + 1u; }
    inline std::size_t right_child_idx(std::size_t i) const noexcept { return (i * 2u) + 2u; }
    static bool less(element_t const& a, element_t const& b) noexcept { return comparator_t{}(a, b); }

    void shift_up(std::size_t i) noexcept {
        for (; i && less(elements_[parent_idx(i)], elements_[i]); i = parent_idx(i))
            std::swap(elements_[parent_idx(i)], elements_[i]);
    }

    void shift_down(std::size_t i) noexcept {
        std::size_t max_idx = i;

        std::size_t left = left_child_idx(i);
        if (left < size_ && less(elements_[max_idx], elements_[left]))
            max_idx = left;

        std::size_t right = right_child_idx(i);
        if (right < size_ && less(elements_[max_idx], elements_[right]))
            max_idx = right;

        if (i != max_idx) {
            std::swap(elements_[i], elements_[max_idx]);
            shift_down(max_idx);
        }
    }
};

/**
 *
 */
class mutex_t {
#if defined(WINDOWS)
    using slot_t = volatile LONG;
#else
    using slot_t = std::int32_t;
#endif // WINDOWS

    slot_t flag_;

  public:
    inline mutex_t(slot_t flag = 0) noexcept : flag_(flag) {}
    inline ~mutex_t() noexcept {}

    inline bool try_lock() noexcept {
        slot_t raw = 0;
#if defined(WINDOWS)
        return InterlockedCompareExchange(&flag_, 1, raw);
#else
        return __atomic_compare_exchange_n(&flag_, &raw, 1, true, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED);
#endif // WINDOWS
    }

    inline void lock() noexcept {
        slot_t raw = 0;
#if defined(WINDOWS)
        InterlockedCompareExchange(&flag_, 1, raw);
#else
    lock_again:
        raw = 0;
        if (!__atomic_compare_exchange_n(&flag_, &raw, 1, true, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED))
            goto lock_again;
#endif // WINDOWS
    }

    inline void unlock() noexcept {
#if defined(WINDOWS)
        InterlockedExchange(&flag_, 0);
#else
        __atomic_store_n(&flag_, 0, __ATOMIC_RELEASE);
#endif
    }
};

static_assert(sizeof(mutex_t) == sizeof(std::int32_t), "Mutex is larger than expected");

using lock_t = std::unique_lock<mutex_t>;

/**
 *  @brief Five-byte integer type to address node clouds with over 4B entries.
 */
#if defined(WINDOWS)
#pragma pack(push, 1) // Pack struct members on 1-byte alignment
#endif

class usearch_pack_m uint40_t {
    unsigned char octets[5];

  public:
    inline uint40_t() noexcept { std::memset(octets, 0, 5); }
    inline uint40_t(std::uint32_t n) noexcept { std::memcpy(octets + 1, (char*)&n, 4), octets[0] = 0; }
    inline uint40_t(std::uint64_t n) noexcept { std::memcpy(octets, (char*)&n + 3, 5); }
#if defined(__clang__)
    inline uint40_t(std::size_t n) noexcept { std::memcpy(octets, (char*)&n + 3, 5); }
#endif

    uint40_t(uint40_t&&) = default;
    uint40_t(uint40_t const&) = default;
    uint40_t& operator=(uint40_t&&) = default;
    uint40_t& operator=(uint40_t const&) = default;

    inline uint40_t& operator+=(std::uint32_t i) noexcept {
        std::uint32_t& tail = *reinterpret_cast<std::uint32_t*>(octets + 1);
        octets[0] += static_cast<unsigned char>((tail + i) < tail);
        tail += i;
        return *this;
    }

    inline operator std::size_t() const noexcept {
        std::size_t result = 0;
        std::memcpy((char*)&result + 3, octets, 5);
        return result;
    }

    inline uint40_t& operator++() noexcept { return *this += 1; }

    inline uint40_t operator++(int) noexcept {
        uint40_t old = *this;
        *this += 1;
        return old;
    }
};
#if defined(WINDOWS)
#pragma pack(pop) // Reset alignment to default
#endif

static_assert(sizeof(uint40_t) == 5, "uint40_t must be exactly 5 bytes");

template <typename scalar_at> class span_gt {
    scalar_at* data_;
    std::size_t size_;

  public:
    span_gt(scalar_at* begin, scalar_at* end) noexcept : data_(begin), size_(end - begin) {}
    span_gt(scalar_at* begin, std::size_t size) noexcept : data_(begin), size_(size) {}
    scalar_at* data() const noexcept { return data_; }
    std::size_t size() const noexcept { return size_; }
    operator scalar_at*() const noexcept { return data(); }
};

/**
 *  @brief
 *      Configuration settings for the index construction.
 *      Includes the main `::connectivity` parameter (`M` in the paper)
 *      and two expansion factors - for construction and search.
 */
struct config_t {

    /// @brief Number of neighbors per graph node.
    /// Defaults to 32 in FAISS and 16 in hnswlib.
    /// > It is called `M` in the paper.
    static constexpr std::size_t connectivity_default_k = 16;

    /// @brief Hyper-parameter controlling the quality of indexing.
    /// Defaults to 40 in FAISS and 200 in hnswlib.
    /// > It is called `efConstruction` in the paper.
    static constexpr std::size_t expansion_add_default_k = 128;

    /// @brief Hyper-parameter controlling the quality of search.
    /// Defaults to 16 in FAISS and 10 in hnswlib.
    /// > It is called `ef` in the paper.
    static constexpr std::size_t expansion_search_default_k = 64;

    std::size_t connectivity = connectivity_default_k;
    std::size_t expansion_add = expansion_add_default_k;
    std::size_t expansion_search = expansion_search_default_k;

    ///
    std::size_t max_elements = 0;
    std::size_t max_threads_add = 0;
    std::size_t max_threads_search = 0;
};

/**
 *  @brief
 *      Approximate Nearest Neighbors Search index using the
 *      Hierarchical Navigable Small World graph algorithm.
 *
 *  @section Features
 *      - Search for vectors of different dimensionality.
 *      - Thread-safe.
 *      - Bring your threads!
 *
 *  @tparam metric_at
 *      A function vector responsible for computing the distance between two vectors.
 *      Must overload the call operator with the following signature:
 *          - `result_type (*) (scalar_t const *, scalar_t const *, std::size_t, std::size_t)`
 *
 *  @tparam label_at
 *      The type of unique labels to assign to vectors.
 *
 *  @tparam id_at
 *      The smallest unsigned integer type to address indexed elements.
 *      Can be a built-in `uint32_t`, `uint64_t`, or our custom `uint40_t`.
 *
 *  @tparam allocator_at
 *      Dynamic memory allocator.
 */
template <typename metric_at = ip_gt<float>,            //
          typename label_at = std::size_t,              //
          typename id_at = std::uint32_t,               //
          typename scalar_at = float,                   //
          typename allocator_at = std::allocator<char>> //
class index_gt {
  public:
    using metric_t = metric_at;
    using scalar_t = scalar_at;
    using label_t = label_at;
    using id_t = id_at;
    using allocator_t = allocator_at;

    using distance_t =
        typename std::result_of<metric_t(scalar_t const*, scalar_t const*, std::size_t, std::size_t)>::type;

  private:
    using neighbors_count_t = id_t;
    using dim_t = std::uint32_t;
    using level_t = std::int32_t;

    using allocator_traits_t = std::allocator_traits<allocator_t>;
    using byte_t = typename allocator_t::value_type;
    static_assert(sizeof(byte_t) == 1, "Allocator must allocate separate addressable bytes");
    static constexpr std::size_t base_level_multiple_k = 2;

    using visits_bitset_t = visits_bitset_gt<allocator_t>;

    struct precomputed_constants_t {
        double inverse_log_connectivity{};
        std::size_t connectivity_max_base{};
        std::size_t neighbors_bytes{};
        std::size_t neighbors_base_bytes{};
        std::size_t mutex_bytes{};
    };
    struct distance_and_id_t {
        distance_t first;
        id_t second;
    };
    struct compare_by_distance_t {
        inline bool operator()(distance_and_id_t a, distance_and_id_t b) const noexcept { return a.first < b.first; }
    };

    using distances_and_ids_allocator_t = typename allocator_traits_t::template rebind_alloc<distance_and_id_t>;
    using distances_and_ids_t = max_heap_gt<distance_and_id_t, compare_by_distance_t, distances_and_ids_allocator_t>;

    struct neighbors_ref_t {
        neighbors_count_t& count_;
        id_t* neighbors_{};

        inline neighbors_ref_t(byte_t* tape) noexcept
            : count_(*(neighbors_count_t*)tape), neighbors_((neighbors_count_t*)tape + 1) {}
        inline id_t* begin() noexcept { return neighbors_; }
        inline id_t* end() noexcept { return neighbors_ + count_; }
        inline id_t const* begin() const noexcept { return neighbors_; }
        inline id_t const* end() const noexcept { return neighbors_ + count_; }
        inline id_t& operator[](std::size_t i) noexcept { return neighbors_[i]; }
        inline id_t operator[](std::size_t i) const noexcept { return neighbors_[i]; }
        inline std::size_t size() const noexcept { return count_; }
    };

#if defined(WINDOWS)
#pragma pack(push, 1) // Pack struct members on 1-byte alignment
#endif
    struct usearch_pack_m node_head_t {
        label_t label;
        dim_t dim;
        level_t level;
        // Variable length array, that has multiple similarly structured segments.
        // Each starts with a `neighbors_count_t` and is followed by such number of `id_t`s.
        byte_t neighbors[1];
    };
#if defined(WINDOWS)
#pragma pack(pop) // Reset alignment to default
#endif

    static constexpr std::size_t head_bytes_k = sizeof(label_t) + sizeof(dim_t) + sizeof(level_t);

    struct node_t {
        byte_t* tape_{};
        scalar_t* vector_{};

        explicit node_t(byte_t* tape, scalar_t* vector) noexcept : tape_(tape), vector_(vector) {}

        node_t() = default;
        node_t(node_t const&) = default;
        node_t& operator=(node_t const&) = default;
    };

    class node_ref_t {
        mutex_t* mutex_{};

      public:
        node_head_t& head{};
        scalar_t* vector{};

        inline node_ref_t(mutex_t& m, node_head_t& h, scalar_t* s) noexcept : mutex_(&m), head(h), vector(s) {}
        inline lock_t lock() const noexcept { return mutex_ ? lock_t{*mutex_} : lock_t{}; }
        inline operator node_t() const noexcept { return node_t{mutex_ ? (byte_t*)mutex_ : (byte_t*)&head, vector}; }
    };

    struct usearch_align_m thread_context_t {
        distances_and_ids_t top_candidates;
        distances_and_ids_t candidates_set;
        visits_bitset_t visits;
        std::default_random_engine level_generator;
        metric_t metric;
    };

    config_t config_{};
    metric_t metric_{};
    allocator_t allocator_{};
    precomputed_constants_t pre_{};
    int viewed_file_descriptor_{};
#if defined(USEARCH_IOURING)
    struct io_uring ring_ {};
#endif

    usearch_align_m mutable std::atomic<std::size_t> capacity_{};
    usearch_align_m mutable std::atomic<std::size_t> size_{};

    mutex_t global_mutex_{};
    level_t max_level_{};
    id_t entry_id_{};
    id_t base_id_{};

    using node_allocator_t = typename allocator_traits_t::template rebind_alloc<node_t>;
    std::vector<node_t, node_allocator_t> nodes_{};

    using thread_context_allocator_t = typename allocator_traits_t::template rebind_alloc<thread_context_t>;
    mutable std::vector<thread_context_t, thread_context_allocator_t> thread_contexts_{};

  public:
    std::size_t connectivity() const noexcept { return config_.connectivity; }
    std::size_t capacity() const noexcept { return capacity_; }
    std::size_t size() const noexcept { return size_; }
    std::size_t max_threads_add() const noexcept { return config_.max_threads_add; }
    bool is_immutable() const noexcept { return viewed_file_descriptor_ != 0; }
    bool synchronize() const noexcept { return config_.max_threads_add > 1; }

    index_gt(config_t config = {}, metric_t metric = {}, allocator_t allocator = {}) noexcept(false)
        : config_(config), metric_(metric), allocator_(allocator) {

        // Externally defined hyper-parameters:
        config_.expansion_add = (std::max)(config_.expansion_add, config_.connectivity);
        pre_ = precompute(config);

        // Configure initial empty state:
        size_ = 0u;
        max_level_ = -1;
        entry_id_ = 0u;
        viewed_file_descriptor_ = 0;

        // Dynamic memory:
        thread_contexts_.resize((std::max)(config.max_threads_search, config.max_threads_add));
        for (thread_context_t& context : thread_contexts_)
            context.metric = metric;
        reserve(config.max_elements);

        // Prefetching:
#if defined(USEARCH_IOURING)
        io_uring_queue_init(config.max_threads_search * pre_.connectivity_max_base, &ring_, 0);
#endif
    }

    index_gt fork() noexcept(false) { return {config_, metric_, allocator_}; }

    ~index_gt() noexcept { clear(); }

    index_gt(index_gt&& other) noexcept { swap(other); }

    index_gt& operator=(index_gt&& other) noexcept {
        swap(other);
        return *this;
    }

#pragma region Adjusting Configuration

    void clear() noexcept {
        std::size_t n = size_;
        for (std::size_t i = 0; i != n; ++i)
            node_free(i);
        size_ = 0;
        max_level_ = -1;
        entry_id_ = 0u;
    }

    void swap(index_gt& other) noexcept {
        std::swap(config_, other.config_);
        std::swap(metric_, other.metric_);
        std::swap(allocator_, other.allocator_);
        std::swap(pre_, other.pre_);
        std::swap(viewed_file_descriptor_, other.viewed_file_descriptor_);
        std::swap(max_level_, other.max_level_);
        std::swap(entry_id_, other.entry_id_);
        std::swap(nodes_, other.nodes_);
        std::swap(thread_contexts_, other.thread_contexts_);

        // Non-atomic parts.
        std::size_t capacity = capacity_;
        std::size_t size = size_;
        capacity_ = other.capacity_.load();
        size_ = other.size_.load();
        other.capacity_ = capacity;
        other.size_ = size;
    }

    void reserve(std::size_t new_capacity) noexcept(false) {

        assert_m(new_capacity >= size_, "Can't drop existing values");
        nodes_.resize(new_capacity);
        for (thread_context_t& context : thread_contexts_)
            context.visits = visits_bitset_t(new_capacity);

        capacity_ = new_capacity;
    }

    static config_t optimize(config_t const& config) noexcept {
        precomputed_constants_t pre = precompute(config);
        std::size_t bytes_per_node_base = head_bytes_k + pre.neighbors_base_bytes + pre.mutex_bytes;
        std::size_t rounded_size = divide_round_up<64>(bytes_per_node_base) * 64;
        std::size_t added_connections = (rounded_size - rounded_size) / sizeof(id_t);
        config_t result = config;
        result.connectivity = config.connectivity + added_connections / base_level_multiple_k;
        return result;
    }

#pragma endregion

#pragma region Construction and Search

    id_t add(                                                //
        label_t new_label, span_gt<scalar_t const> new_span, //
        std::size_t thread_idx = 0, bool store_vector = true) {

        assert_m(!is_immutable(), "Can't add to an immutable index");
        id_t new_id = static_cast<id_t>(size_.fetch_add(1));
        scalar_t const* new_vector = new_span.data();
        std::size_t new_dim = new_span.size();

        // Determining how much memory to allocate depends on the target level.
        lock_t new_level_lock(global_mutex_);
        level_t max_level = max_level_;
        thread_context_t& context = thread_contexts_[thread_idx];
        level_t new_target_level = choose_random_level(context.level_generator);
        if (new_target_level <= max_level)
            new_level_lock.unlock();

        // Allocate the neighbors
        node_ref_t new_node = node_malloc(new_label, new_vector, new_dim, new_target_level, store_vector);
        lock_t new_lock = new_node.lock();
        nodes_[new_id] = new_node;

        // Do nothing for the first element
        if (!new_id) {
            max_level_ = new_target_level;
            return new_id;
        }

        // Go down the level, tracking only the closest match
        id_t closest_id = search_for_one(entry_id_, new_vector, new_dim, max_level, new_target_level, context);

        // From `new_target_level` down perform proper extensive search.
        for (level_t level = (std::min)(new_target_level, max_level); level >= 0; level--) {
            search_to_insert(closest_id, new_vector, new_dim, level, context);
            closest_id = connect_new_element(new_id, level, context);
        }

        // Releasing lock for the maximum level
        if (new_target_level > max_level) {
            entry_id_ = new_id;
            max_level_ = new_target_level;
        }
        return new_id;
    }

    template <typename label_and_distance_callback_at>
    void search(                                                //
        span_gt<scalar_t const> query_span, std::size_t wanted, //
        label_and_distance_callback_at&& callback, std::size_t thread_idx = 0) const {

        if (!size_)
            return;

        scalar_t const* query_vec = query_span.data();
        std::size_t query_dim = query_span.size();

        // Go down the level, tracking only the closest match
        thread_context_t& context = thread_contexts_[thread_idx];
        id_t closest_id = search_for_one(entry_id_, query_vec, query_dim, max_level_, 0, context);

        // For bottom layer we need a more optimized procedure
        search_to_find_in_base( //
            closest_id, query_vec, query_dim, (std::max)(config_.expansion_search, wanted), context);
        while (context.top_candidates.size() > wanted)
            context.top_candidates.pop();

        while (context.top_candidates.size()) {
            distance_and_id_t top = context.top_candidates.top();
            callback(node(top.second).head.label, top.first);
            context.top_candidates.pop();
        }
    }

    std::size_t search(                                         //
        span_gt<scalar_t const> query_span, std::size_t wanted, //
        label_t* matches, distance_t* distances,                //
        std::size_t thread_idx = 0) const {

        std::size_t found = 0;
        auto callback = [&](label_t label, distance_t distance) noexcept {
            if (matches)
                matches[found] = label;
            if (distances)
                distances[found] = distance;
            ++found;
        };
        search(query_span, wanted, callback, thread_idx);
        std::reverse(matches, matches + found * (matches != nullptr));
        std::reverse(distances, distances + found * (distances != nullptr));
        return found;
    }

    /**
     *  @brief  Assuming minor point drift, adjusts the coordinates
     *          of a point, reassembling a new neighbors lists for it
     *          and it's older neighbors.
     */
    void update(id_t existing_id, span_gt<scalar_t const> new_span, //
                std::size_t thread_idx = 0, bool store_vector = true) {}

    struct extracted_node_t {
        node_t node;
        index_gt&& source;
    };

    extracted_node_t extract();

    /**
     *  @brief  Imports a separate index.
     *
     *  @warning Not thread-safe!
     */
    std::size_t merge(index_gt&& imported) {
        // For every point in the `imported` index - find nearest neighbors in existing.
        return 0;
    }

#pragma endregion

#pragma region Serialization

    struct state_t {
        // Check compatibility
        std::uint64_t bytes_per_label{};
        std::uint64_t bytes_per_id{};

        // Describe state
        std::uint64_t connectivity{};
        std::uint64_t size{};
        std::uint64_t entry_id{};
        std::uint64_t max_level{};
    };

    /**
     *  Compatibility: Linux, MacOS, Windows.
     */
    void save(char const* file_path) const noexcept(false) {

        state_t state;
        // Check compatibility
        state.bytes_per_label = sizeof(label_t);
        state.bytes_per_id = sizeof(id_t);
        // Describe state
        state.connectivity = config_.connectivity;
        state.size = size_;
        state.entry_id = entry_id_;
        state.max_level = max_level_;

        std::FILE* file = std::fopen(file_path, "w");
        if (!file)
            throw std::runtime_error(std::strerror(errno));

        // Write the header
        {
            std::size_t written = std::fwrite(&state, sizeof(state), 1, file);
            if (!written) {
                std::fclose(file);
                throw std::runtime_error(std::strerror(errno));
            }
        }

        // Serialize nodes one by one
        for (std::size_t i = 0; i != state.size; ++i) {
            node_ref_t node_ref = node(static_cast<id_t>(i));
            std::size_t bytes_to_dump = node_dump_size(node_ref.head.dim, node_ref.head.level);
            std::size_t bytes_in_vec = node_ref.head.dim * sizeof(scalar_t);
            // Dump just neighbors, as vectors may be in a disjoint location
            std::size_t written = std::fwrite(&node_ref.head, bytes_to_dump - bytes_in_vec, 1, file);
            if (!written) {
                std::fclose(file);
                throw std::runtime_error(std::strerror(errno));
            }
            // Dump the vector
            written = std::fwrite(node_ref.vector, bytes_in_vec, 1, file);
            if (!written) {
                std::fclose(file);
                throw std::runtime_error(std::strerror(errno));
            }
        }

        std::fclose(file);
    }

    /**
     *  Compatibility: Linux, MacOS, Windows.
     */
    void load(char const* file_path) noexcept(false) {
        state_t state;
        std::FILE* file = std::fopen(file_path, "r");
        if (!file)
            throw std::runtime_error(std::strerror(errno));

        // Read the header
        {
            std::size_t read = std::fread(&state, sizeof(state), 1, file);
            if (!read) {
                std::fclose(file);
                throw std::runtime_error(std::strerror(errno));
            }
            if (state.bytes_per_label != sizeof(label_t)) {
                std::fclose(file);
                throw std::runtime_error("Incompatible label type!");
            }
            if (state.bytes_per_id != sizeof(id_t)) {
                std::fclose(file);
                throw std::runtime_error("Incompatible ID type!");
            }

            config_.connectivity = state.connectivity;
            config_.max_elements = state.size;
            pre_ = precompute(config_);
            reserve(state.size);
            size_ = state.size;
            max_level_ = static_cast<level_t>(state.max_level);
            entry_id_ = static_cast<id_t>(state.entry_id);
        }

        // Load nodes one by one
        for (std::size_t i = 0; i != state.size; ++i) {
            node_head_t head;
            std::size_t read = std::fread(&head, head_bytes_k, 1, file);
            if (!read) {
                std::fclose(file);
                throw std::runtime_error(std::strerror(errno));
            }

            std::size_t bytes_to_dump = node_dump_size(head.dim, head.level);
            node_ref_t node_ref = node_malloc(head.label, nullptr, head.dim, head.level, true);
            read = std::fread((byte_t*)&node_ref.head + head_bytes_k, bytes_to_dump - head_bytes_k, 1, file);
            if (!read) {
                std::fclose(file);
                throw std::runtime_error(std::strerror(errno));
            }

            nodes_[i] = node_ref;
        }

        std::fclose(file);
        viewed_file_descriptor_ = 0;
    }

    /**
     *  Compatibility: Linux, MacOS.
     */
    void view(char const* file_path) noexcept(false) {
#if defined(WIN32) || defined(_WIN32) || defined(__WIN32__) || defined(__NT__)
        throw std::logic_error("Memory-mapping is not yet available for Windows");
#else
        state_t state;
        int open_flags = O_RDONLY;
#if __linux__
        open_flags |= O_NOATIME;
#endif
        int descriptor = open(file_path, open_flags);

        // Estimate the file size
        struct stat file_stat;
        int fstat_status = fstat(descriptor, &file_stat);
        if (fstat_status < 0) {
            close(descriptor);
            throw std::runtime_error(std::strerror(errno));
        }

        // Map the entire file
        byte_t* file = (byte_t*)mmap(NULL, file_stat.st_size, PROT_READ, MAP_PRIVATE, descriptor, 0);
        if (file == MAP_FAILED) {
            close(descriptor);
            throw std::runtime_error(std::strerror(errno));
        }

        // Read the header
        {
            std::memcpy(&state, file, sizeof(state));
            if (state.bytes_per_label != sizeof(label_t)) {
                close(descriptor);
                throw std::runtime_error("Incompatible label type!");
            }
            if (state.bytes_per_id != sizeof(id_t)) {
                close(descriptor);
                throw std::runtime_error("Incompatible ID type!");
            }

            config_.connectivity = state.connectivity;
            config_.max_elements = state.size;
            config_.max_threads_add = 0;
            pre_ = precompute(config_);
            reserve(state.size);
            size_ = state.size;
            max_level_ = static_cast<level_t>(state.max_level);
            entry_id_ = static_cast<id_t>(state.entry_id);
        }

        // Locate every node packed into file
        std::size_t progress = sizeof(state);
        for (std::size_t i = 0; i != state.size; ++i) {
            node_head_t const& head = *(node_head_t const*)(file + progress);
            std::size_t bytes_to_dump = node_dump_size(head.dim, head.level);
            std::size_t bytes_in_vec = head.dim * sizeof(scalar_t);
            nodes_[i].tape_ = (byte_t*)(file + progress);
            nodes_[i].vector_ = (scalar_t*)(file + progress + bytes_to_dump - bytes_in_vec);
            progress += bytes_to_dump;
            max_level_ = (std::max)(max_level_, head.level);
        }

        bool replaced_existing_map = viewed_file_descriptor_ != 0;
        viewed_file_descriptor_ = descriptor;
        (void)replaced_existing_map;

#if defined(USEARCH_IOURING)
        io_uring_register(ring_., replaced_existing_map ? IORING_REGISTER_FILES : IORING_REGISTER_FILES_UPDATE,
                          &viewed_file_descriptor_, 1);
#endif // USEARCH_IOURING
#endif // POSIX
    }

#pragma endregion

  private:
    inline static precomputed_constants_t precompute(config_t const& config) noexcept {
        precomputed_constants_t pre;
        pre.connectivity_max_base = config.connectivity * base_level_multiple_k;
        pre.inverse_log_connectivity = 1.0 / std::log(static_cast<double>(config.connectivity));
        pre.neighbors_bytes = config.connectivity * sizeof(id_t) + sizeof(neighbors_count_t);
        pre.neighbors_base_bytes = pre.connectivity_max_base * sizeof(id_t) + sizeof(neighbors_count_t);
        pre.mutex_bytes = sizeof(mutex_t) * (config.max_threads_add > 1);
        return pre;
    }

    inline std::size_t node_dump_size(dim_t dim, level_t level) const noexcept {
        return head_bytes_k + pre_.neighbors_base_bytes + pre_.neighbors_bytes * level + sizeof(scalar_t) * dim;
    }

    void node_free(std::size_t id) noexcept {

        if (viewed_file_descriptor_ != 0)
            return;

        // This function is rarely called and can be as expensive as needed for higher space-efficiency.
        node_t& node = nodes_[id];
        if (!node.tape_)
            return;

        node_head_t const& head = *(node_head_t const*)(node.tape_ + pre_.mutex_bytes);
        std::size_t levels_bytes = pre_.neighbors_base_bytes + pre_.neighbors_bytes * head.level;
        bool store_vector = (byte_t*)(node.tape_ + pre_.mutex_bytes + head_bytes_k + levels_bytes) == //
                            (byte_t*)(node.vector_);
        std::size_t node_bytes =          //
            pre_.mutex_bytes +            // Optional concurrency-control
            head_bytes_k + levels_bytes + // Obligatory neighborhood index
            head.dim * store_vector       // Optional vector copy
            ;

        allocator_t{}.deallocate(node.tape_, node_bytes);
        node = node_t{};
    }

    node_ref_t node_malloc(                                     //
        label_t label, scalar_t const* vector, std::size_t dim, //
        level_t level, bool store_vector = true) noexcept(false) {

        // This function is rarely called and can be as expensive as needed for higher space-efficiency.
        std::size_t levels_bytes = pre_.neighbors_base_bytes + pre_.neighbors_bytes * level;
        std::size_t node_bytes =                  //
            pre_.mutex_bytes +                    // Optional concurrency-control
            head_bytes_k + levels_bytes +         // Obligatory neighborhood index
            sizeof(scalar_t) * dim * store_vector // Optional vector copy
            ;

        byte_t* data = (byte_t*)allocator_t{}.allocate(node_bytes);
        assert_m(data, "Not enough memory for links");

        mutex_t* mutex = synchronize() ? (mutex_t*)data : nullptr;
        scalar_t* scalars = store_vector //
                                ? (scalar_t*)(data + pre_.mutex_bytes + head_bytes_k + levels_bytes)
                                : (scalar_t*)(vector);

        std::memset(data, 0, node_bytes);
        std::memcpy(scalars, vector, sizeof(scalar_t) * dim * (store_vector && vector));

        node_head_t& head = *(node_head_t*)(data + pre_.mutex_bytes);
        head.label = label;
        head.dim = static_cast<dim_t>(dim);
        head.level = level;

        return {*mutex, head, scalars};
    }

    inline node_ref_t node(id_t id) const noexcept { return node(nodes_[id]); }

    inline node_ref_t node(node_t node) const noexcept {
        byte_t* data = node.tape_;
        mutex_t* mutex = synchronize() ? (mutex_t*)data : nullptr;
        node_head_t& head = *(node_head_t*)(data + pre_.mutex_bytes);
        scalar_t* scalars = node.vector_;

        return {*mutex, head, scalars};
    }

    inline neighbors_ref_t neighbors_base(node_ref_t node) const noexcept { return {node.head.neighbors}; }

    inline neighbors_ref_t neighbors_non_base(node_ref_t node, level_t level) const noexcept {
        return {node.head.neighbors + pre_.neighbors_base_bytes + (level - 1) * pre_.neighbors_bytes};
    }

    inline neighbors_ref_t neighbors(node_ref_t node, level_t level) const noexcept {
        return level ? neighbors_non_base(node, level) : neighbors_base(node);
    }

    id_t connect_new_element(id_t new_id, level_t level, thread_context_t& context) noexcept(false) {

        node_ref_t new_node = node(new_id);
        distances_and_ids_t& top_candidates = context.top_candidates;
        std::size_t const connectivity_max = level ? config_.connectivity : pre_.connectivity_max_base;
        span_gt<distance_and_id_t const> top = filter_heuristic(top_candidates, config_.connectivity, context.metric);

        distance_and_id_t const* const top_ordered = top.data();
        std::size_t const top_count = top.size();
        id_t const next_closest_entry_id = top_ordered[0].second;

        // Outgoing links from `new_id`:
        {
            neighbors_ref_t new_neighbors = neighbors(new_node, level);
            assert_m(!new_neighbors.count_, "The newly inserted element should have blank link list");

            new_neighbors.count_ = static_cast<neighbors_count_t>(top_count);
            for (std::size_t idx = 0; idx < top_count; idx++) {
                assert_m(!new_neighbors[idx], "Possible memory corruption");
                assert_m(level <= node(top_ordered[idx].second).head.level, "Linking to missing level");
                new_neighbors[idx] = top_ordered[idx].second;
            }
        }

        // Reverse links from the neighbors:
        for (std::size_t idx = 0; idx < top_count; idx++) {
            id_t close_id = top_ordered[idx].second;
            node_ref_t close_node = node(close_id);
            lock_t close_lock = close_node.lock();

            neighbors_ref_t close_header = neighbors(close_node, level);
            assert_m(close_header.count_ <= connectivity_max, "Possible corruption");
            assert_m(close_id != new_id, "Self-loops are impossible");
            assert_m(level <= close_node.head.level, "Linking to missing level");

            // If `new_id` is already present in the neighboring connections of `close_id`
            // then no need to modify any connections or run the heuristics.
            if (close_header.count_ < connectivity_max) {
                close_header[close_header.count_] = new_id;
                close_header.count_++;
                continue;
            }

            // To fit a new connection we need to drop an existing one.
            distances_and_ids_t& candidates = context.candidates_set;
            candidates.clear();
            candidates.emplace( //
                context.metric( //
                    new_node.vector, close_node.vector, new_node.head.dim, close_node.head.dim),
                new_id);
            for (id_t successor_id : close_header) {
                node_ref_t successor_node = node(successor_id);
                candidates.emplace( //
                    context.metric( //
                        successor_node.vector, close_node.vector, successor_node.head.dim, close_node.head.dim),
                    successor_id);
            }
            span_gt<distance_and_id_t const> top = filter_heuristic(candidates, connectivity_max, context.metric);

            // Export the results:
            for (close_header.count_ = 0u; close_header.count_ != top.size(); ++close_header.count_)
                close_header[close_header.count_] = top[close_header.count_].second;
        }

        return next_closest_entry_id;
    }

    level_t choose_random_level(std::default_random_engine& level_generator) const noexcept {
        std::uniform_real_distribution<double> distribution(0.0, 1.0);
        double r = -std::log(distribution(level_generator)) * pre_.inverse_log_connectivity;
        return (level_t)r;
    }

    id_t search_for_one(                                                 //
        id_t entry_id, scalar_t const* query_vec, std::size_t query_dim, //
        level_t begin_level, level_t end_level, thread_context_t& context) const noexcept {

        id_t closest_id = entry_id;
        distance_t closest_dist =
            context.metric(query_vec, node(closest_id).vector, query_dim, node(closest_id).head.dim);
        for (level_t level = begin_level; level > end_level; level--) {
            bool changed;
            do {
                changed = false;
                node_ref_t closest_node = node(closest_id);
                lock_t closest_lock = closest_node.lock();
                neighbors_ref_t closest_header = neighbors_non_base(closest_node, level);
                for (id_t candidate_id : closest_header) {
                    node_ref_t candidate_node = node(candidate_id);
                    distance_t candidate_dist =
                        context.metric(query_vec, candidate_node.vector, query_dim, candidate_node.head.dim);
                    if (candidate_dist < closest_dist) {
                        closest_dist = candidate_dist;
                        closest_id = candidate_id;
                        changed = true;
                    }
                }
            } while (changed);
        }
        return closest_id;
    }

    void search_to_insert(                                               //
        id_t start_id, scalar_t const* query_vec, std::size_t query_dim, //
        level_t level, thread_context_t& context) noexcept(false) {

        visits_bitset_t& visits = context.visits;
        distances_and_ids_t& top_candidates = context.top_candidates; // pop max, push
        distances_and_ids_t& candidates_set = context.candidates_set; // pop min, push

        top_candidates.clear();
        candidates_set.clear();
        visits.clear();

        distance_t closest_dist = context.metric(query_vec, node(start_id).vector, query_dim, node(start_id).head.dim);
        top_candidates.emplace(closest_dist, start_id);
        candidates_set.emplace(-closest_dist, start_id);
        visits.set(start_id);

        while (!candidates_set.empty()) {

            distance_and_id_t candidacy = candidates_set.top();
            if ((-candidacy.first) > closest_dist && top_candidates.size() == config_.expansion_add)
                break;

            candidates_set.pop();
            id_t candidate_id = candidacy.second;
            node_ref_t candidate_node = node(candidate_id);
            lock_t candidate_lock = candidate_node.lock();
            neighbors_ref_t candidate_header = neighbors(candidate_node, level);

            prefetch_neighbors(candidate_header, visits);
            for (id_t successor_id : candidate_header) {
                if (visits.test(successor_id))
                    continue;

                visits.set(successor_id);
                node_ref_t successor_node = node(successor_id);
                distance_t successor_dist =
                    context.metric(query_vec, successor_node.vector, query_dim, successor_node.head.dim);
                if (top_candidates.size() < config_.expansion_add || closest_dist > successor_dist) {
                    candidates_set.emplace(-successor_dist, successor_id);

                    top_candidates.emplace(successor_dist, successor_id);
                    if (top_candidates.size() > config_.expansion_add)
                        top_candidates.pop();
                    if (!top_candidates.empty())
                        closest_dist = top_candidates.top().first;
                }
            }
        }
    }

    void search_to_find_in_base(                                         //
        id_t start_id, scalar_t const* query_vec, std::size_t query_dim, //
        std::size_t expansion, thread_context_t& context) const noexcept(false) {

        visits_bitset_t& visits = context.visits;
        distances_and_ids_t& top_candidates = context.top_candidates; // pop max, push
        distances_and_ids_t& candidates_set = context.candidates_set; // pop min, push

        visits.clear();
        top_candidates.clear();
        candidates_set.clear();

        distance_t closest_dist = context.metric(query_vec, node(start_id).vector, query_dim, node(start_id).head.dim);
        top_candidates.emplace(closest_dist, start_id);
        candidates_set.emplace(-closest_dist, start_id);
        visits.set(start_id);

        while (!candidates_set.empty()) {

            distance_and_id_t current_node_node = candidates_set.top();
            if ((-current_node_node.first) > closest_dist)
                break;

            candidates_set.pop();

            id_t candidate_id = current_node_node.second;
            neighbors_ref_t candidate_header = neighbors_base(node(candidate_id));

            prefetch_neighbors(candidate_header, visits);
            for (id_t successor_id : candidate_header) {
                if (visits.test(successor_id))
                    continue;

                visits.set(successor_id);
                node_ref_t successor_node = node(successor_id);
                distance_t successor_dist =
                    context.metric(query_vec, successor_node.vector, query_dim, successor_node.head.dim);

                if (top_candidates.size() < expansion || closest_dist > successor_dist) {
                    candidates_set.emplace(-successor_dist, successor_id);

                    top_candidates.emplace(successor_dist, successor_id);
                    if (top_candidates.size() > expansion)
                        top_candidates.pop();
                    if (!top_candidates.empty())
                        closest_dist = top_candidates.top().first;
                }
            }
        }
    }

    void prefetch_neighbors(neighbors_ref_t head, visits_bitset_t const& visits) const noexcept {

        // Prefetch from disk
        if (viewed_file_descriptor_ != 0)
            for (id_t successor_id : head) {
                if (visits.test(successor_id))
                    continue;

                node_ref_t node_ref = node(successor_id);
                node_head_t& head = node_ref.head;

                // Naive
                __builtin_prefetch(node(successor_id).vector);

                // Old-school
                // std::size_t length = node_dump_size(head.dim, 0);
                // madvise(&head, length, MADV_WILLNEED);

                // Async
#if defined(USEARCH_IOURING)
                struct io_uring_sqe* sqe = io_uring_get_sqe(ring_);
                io_uring_prep_madvice(sqe, &head, length, MADV_WILLNEED);
                io_uring_sqe_set_data(sqe, fi);
                io_uring_submit(ring_);
#endif
            }
        else
            for (id_t successor_id : head) {
                if (!visits.test(successor_id))
                    __builtin_prefetch(node(successor_id).vector);
            }
    }

    span_gt<distance_and_id_t const> filter_heuristic( //
        distances_and_ids_t& top_candidates, std::size_t needed, metric_t const& metric) const noexcept {

        top_candidates.sort_ascending();
        distance_and_id_t* top_ordered = top_candidates.data();
        std::size_t const top_count = top_candidates.size();
        if (top_count < needed)
            return {top_ordered, top_count};

        std::size_t submitted_count = 1;
        std::size_t consumed_count = 1; /// Always equal or greater than `submitted_count`.
        while (submitted_count < needed && consumed_count < top_count) {
            distance_and_id_t candidate = top_ordered[consumed_count];
            node_ref_t candidate_node = node(candidate.second);
            distance_t candidate_dist = candidate.first;
            bool good = true;
            for (std::size_t idx = 0; idx < submitted_count; idx++) {
                distance_and_id_t submitted = top_ordered[idx];
                node_ref_t submitted_node = node(submitted.second);
                distance_t inter_result_dist = metric(            //
                    submitted_node.vector, candidate_node.vector, //
                    submitted_node.head.dim, candidate_node.head.dim);
                if (inter_result_dist < candidate_dist) {
                    good = false;
                    break;
                }
            }

            if (good) {
                top_ordered[submitted_count] = top_ordered[consumed_count];
                submitted_count++;
            }
            consumed_count++;
        }

        return {top_ordered, submitted_count};
    }
};

} // namespace usearch
} // namespace unum

#endif
