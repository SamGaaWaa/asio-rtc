#pragma once

#include <concepts>
#include <ranges>

namespace asiortc {

template <class T>
concept VectorLikeBuffer = requires (T &t, std::size_t n) {
    t.resize(n);
} && std::ranges::contiguous_range<T> && (sizeof(typename T::value_type) == 1);

} // namespace asiortc