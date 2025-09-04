//
//  retryxx_retry.h
//  retryxx
//
//  MIT License
//
//  Copyright (c) 2025 Jacob Sologub
//
//  Permission is hereby granted, free of charge, to any person obtaining a copy
//  of this software and associated documentation files (the "Software"), to deal
//  in the Software without restriction, including without limitation the rights
//  to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
//  copies of the Software, and to permit persons to whom the Software is
//  furnished to do so, subject to the following conditions:
//
//  The above copyright notice and this permission notice shall be included in all
//  copies or substantial portions of the Software.
//
//  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
//  IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
//  FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
//  AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
//  LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
//  OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
//  SOFTWARE.
//

#pragma once

static_assert (__cplusplus >= 202002L, "retryxx requires C++20 or later");

#include <chrono>
#include <random>
#include <thread>
#include <functional>
#include <type_traits>
#include <concepts>
#include <format>

#if __cpp_lib_expected >= 202202L
 #include <expected>
 namespace retryxx
 {
     template<typename T, typename E>
     using expected = std::expected<T, E>;

     template<typename E>
     using unexpected = std::unexpected<E>;
 }
#elif __has_include(<tl/expected.hpp>)
 #include <tl/expected.hpp>
 namespace retryxx
 {
     template<typename T, typename E>
     using expected = tl::expected<T, E>;

     template<typename E>
     using unexpected = tl::unexpected<E>;
 }
#else
 #error "retryxx requires either C++23 std::expected or tl::expected library (https://github.com/TartanLlama/expected)"
#endif

namespace retryxx
{

template <typename F, typename... Args>
concept Retryable = std::is_invocable_v<F, Args...>;

/// Configures the exponential backoff timing strategy for retry operations.
/// Uses exponential growth with jitter to prevent synchronized retry attempts
/// across multiple clients (thundering herd problem).
struct BackoffPolicy
{
    std::chrono::milliseconds initialDelay;
    double multiplier;
    std::chrono::milliseconds maxDelay;
    mutable std::mt19937_64 rng;

    /// Creates a backoff policy with the specified timing parameters.
    /// @param initial    Starting delay for first retry (default: 1 second)
    /// @param mult       Multiplier for exponential growth (default: 2.0)
    /// @param max        Maximum delay cap (default: 5 minutes)
    BackoffPolicy (std::chrono::milliseconds initial = std::chrono::seconds (1),
                   double mult = 2.0,
                   std::chrono::milliseconds max = std::chrono::minutes (5))
      : initialDelay (initial),
        multiplier (mult),
        maxDelay (max)
    {
        rng.seed (std::random_device{}());
    }

    /// Calculates the randomized delay for the given retry attempt.
    /// @param attempt   The retry attempt number (1-based)
    /// @returns         Random delay between 0 and the calculated exponential delay
    std::chrono::milliseconds getDelay (int attempt) const
    {
        std::chrono::milliseconds currentDelay = initialDelay;
        for (int i = 0; i < attempt - 1; ++i)
        {
            currentDelay = std::min (currentDelay * static_cast<long long> (multiplier), maxDelay);
        }

        std::uniform_int_distribution<long long> dist (0, currentDelay.count());
        return std::chrono::milliseconds (dist (rng));
    }
};

/// Executes a function with retry logic using exponential backoff and jitter.
/// The function is retried based on both its return value and any exceptions thrown.
/// @param func                             The function to execute and potentially retry
/// @param shouldRetryPredicate             Determines if result should trigger a retry
/// @param shouldRetryExceptionPredicate    Determines if exception should trigger a retry
/// @param maxAttempts                      Maximum number of retry attempts
/// @param backoffPolicy                    Timing configuration for retries
/// @returns                                Expected containing either the successful result or error message
template <Retryable F, typename ShouldRetryPredicate,
                       typename ShouldRetryExceptionPredicate,
                       typename ResultType = std::invoke_result_t<F>>
expected<ResultType, std::string> retry (F&& func, 
                                         ShouldRetryPredicate&& shouldRetryPredicate,
                                         ShouldRetryExceptionPredicate&& shouldRetryExceptionPredicate,
                                         int maxAttempts = 5,
                                         BackoffPolicy backoffPolicy = BackoffPolicy{})
{
    for (int attempts = 0; attempts < maxAttempts; ++attempts)
    {
        try
        {
            if (attempts > 0)
            {
                std::this_thread::sleep_for (backoffPolicy.getDelay (attempts));
            }

            ResultType result = std::invoke (std::forward<F> (func));
            if (! shouldRetryPredicate (result))
            {
                return result;
            }
        }
        catch (const std::exception& e)
        {
            if (! shouldRetryExceptionPredicate (e))
            {
                return unexpected (std::format ("Retry failed with exception: {}", e.what()));
            }
        }
    }

    return unexpected (std::format ("Retry failed after {} attempts.", maxAttempts));
}

} // namespace retryxx
