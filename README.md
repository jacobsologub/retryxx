# retryxx

Simple header-only C++20/23 retry mechanism with exponential backoff and jitter.

## Quick Start

```cpp
#include <retryxx/retryxx_retry.h>

auto result = retryxx::retry ([]() 
{
    // Your operation that might fail
    return makeNetworkCall();
},
[] (const auto statusCode) {
    return statusCode != 200; // retry if not successful
},
[] (const std::exception& e) {
    return true; // retry on all exceptions
});

if (result) 
{
    std::cout << "Success: " << result.value() << std::endl;
} 
else 
{
    std::cout << "Failed: " << result.error() << std::endl;
}
```

## Installation

Header-only library. Copy `retryxx_retry.h` to your project and include it.

## Requirements

- C++20 or later
- For C++20: requires [tl::expected](https://github.com/TartanLlama/expected)
- For C++23: uses standard `std::expected`

## How It Works

- **Exponential backoff**: 1s → 2s → 4s → 8s → 16s (doubles each attempt)
- **Jitter**: Randomizes delays to prevent thundering herd problems
- **Configurable**: Custom backoff policies and retry conditions