#pragma once

#include <expected>

namespace orbit {

template <typename T, typename E> using Result = std::expected<T, E>;

template <typename E> using Status = std::expected<void, E>;

} // namespace orbit
