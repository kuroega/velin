// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// std::from_chars with one Apple fallback: Apple's libc++ only ships the
// floating-point overloads on iOS 26 / macOS 26, so older iPadOS releases
// parse floating-point text with strtod_l in the C locale instead, rejecting
// the inputs from_chars rejects (leading space, '+', hexadecimal).

#include <charconv>
#include <string>
#include <system_error>
#include <type_traits>

#if defined(__APPLE__)
#include <cerrno>
#include <cstdlib>
#include <xlocale.h>
#endif

namespace velin::detail {

template <typename Number>
std::from_chars_result fromChars(const char* first, const char* last, Number& value) {
#if defined(__APPLE__)
  if constexpr (std::is_floating_point_v<Number>) {
    const char* digits = first != last && *first == '-' ? first + 1 : first;
    if (digits == last || *digits == '+' || *digits == ' ' || (*digits >= '\t' && *digits <= '\r') ||
        (*digits == '0' && digits + 1 != last && (digits[1] == 'x' || digits[1] == 'X')))
      return {first, std::errc::invalid_argument};
    const std::string text(first, last);
    char* end = nullptr;
    errno = 0;
    Number parsed{};
    if constexpr (std::is_same_v<Number, float>) parsed = strtof_l(text.c_str(), &end, LC_C_LOCALE);
    else if constexpr (std::is_same_v<Number, double>) parsed = strtod_l(text.c_str(), &end, LC_C_LOCALE);
    else parsed = strtold_l(text.c_str(), &end, LC_C_LOCALE);
    if (end == text.c_str()) return {first, std::errc::invalid_argument};
    const char* stop = first + (end - text.c_str());
    if (errno == ERANGE) return {stop, std::errc::result_out_of_range};
    value = parsed;
    return {stop, std::errc{}};
  } else
#endif
  {
    return std::from_chars(first, last, value);
  }
}

}  // namespace velin::detail
