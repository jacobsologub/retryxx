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
#include <stop_token>

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

#if __cpp_lib_jthread >= 201911L
#include <stop_token>

namespace retryxx
{
    using stop_token  = std::stop_token;
    using stop_source = std::stop_source;
}

#else
#include <atomic>

namespace retryxx
{
/// Fallback implementation of std::stop_token for C++20 compatibility.
/// Provides cooperative cancellation mechanism when std::stop_token is unavailable.
class stop_token
{
public:
    stop_token() = default;
    explicit stop_token (const std::atomic<bool>* flag) : stopFlag (flag) {}

    /// Returns true if cancellation has been requested
    bool stop_requested() const noexcept { return stopFlag && stopFlag->load(); }

    /// Returns true if this token is associated with a stop source
    bool stop_possible() const noexcept  { return stopFlag != nullptr; }

private:
    const std::atomic<bool>* stopFlag = nullptr;
};

/// Fallback implementation of std::stop_source for C++20 compatibility.
/// Manages the cancellation state and provides tokens for cooperative cancellation.
class stop_source
{
public:
    stop_source() = default;

    /// Requests cancellation for all associated tokens
    void request_stop() noexcept    { stopped.store (true); }

    /// Returns a token that can be used to check for cancellation
    stop_token get_token() noexcept { return stop_token (&stopped); }

private:
    std::atomic<bool> stopped { false };
};

} // namespace retryxx

#endif

namespace retryxx::detail
{

bool interruptibleSleep (std::chrono::milliseconds duration, stop_token stopToken);

} // namespace detail

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
/// @param stopToken                        Token for cooperative cancellation of retry operation
/// @returns                                Expected containing either the successful result or error message
template <Retryable F, typename ShouldRetryPredicate,
                       typename ShouldRetryExceptionPredicate,
                       typename ResultType = std::invoke_result_t<F>>
expected<ResultType, std::string> retry (F&& func,
                                         ShouldRetryPredicate&& shouldRetryPredicate,
                                         ShouldRetryExceptionPredicate&& shouldRetryExceptionPredicate,
                                         int maxAttempts = 5,
                                         BackoffPolicy backoffPolicy = BackoffPolicy{},
                                         stop_token stopToken = stop_token{})
{
    for (int attempts = 0; attempts < maxAttempts; ++attempts)
    {
        try
        {
            if (attempts > 0)
            {
                if (detail::interruptibleSleep (backoffPolicy.getDelay (attempts), stopToken))
                {
                    return unexpected ("Retry operation was cancelled during backoff.");
                }
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

namespace retryxx::detail
{

inline bool interruptibleSleep (std::chrono::milliseconds duration, stop_token stopToken)
{
    if (! stopToken.stop_possible())
    {
        std::this_thread::sleep_for (duration);
        return false;
    }

    auto sleepIncrement = std::chrono::milliseconds (10);
    auto remaining = duration;

    while (remaining > std::chrono::milliseconds (0) && ! stopToken.stop_requested())
    {
        auto sleepTime = std::min (sleepIncrement, remaining);
        std::this_thread::sleep_for (sleepTime);
        remaining -= sleepTime;
    }

    return stopToken.stop_requested();
}

} // namespace detail
