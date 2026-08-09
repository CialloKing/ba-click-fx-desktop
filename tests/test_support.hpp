#pragma once

#include <cmath>
#include <source_location>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace bafx::test
{

using TestFunction = void (*)();

struct TestCase
{
    std::string_view name;
    TestFunction function;
};

[[nodiscard]] std::vector<TestCase>& registry();

class Registrar
{
public:
    Registrar(const std::string_view name, const TestFunction function)
    {
        registry().push_back(TestCase{name, function});
    }
};

inline void check(
    const bool condition,
    const std::string_view expression,
    const std::source_location location = std::source_location::current())
{
    if (!condition)
    {
        throw std::runtime_error(
            std::string(location.file_name()) + ":"
            + std::to_string(location.line()) + ": check failed: "
            + std::string(expression));
    }
}

inline void checkNear(
    const float actual,
    const float expected,
    const float tolerance,
    const std::source_location location = std::source_location::current())
{
    if (!std::isfinite(actual) || std::abs(actual - expected) > tolerance)
    {
        throw std::runtime_error(
            std::string(location.file_name()) + ":"
            + std::to_string(location.line()) + ": expected "
            + std::to_string(expected) + ", got " + std::to_string(actual));
    }
}

}

#define BAFX_TEST(name) \
    static void name(); \
    static const ::bafx::test::Registrar name##_registrar{#name, &name}; \
    static void name()

#define BAFX_CHECK(expression) \
    ::bafx::test::check(static_cast<bool>(expression), #expression)

#define BAFX_CHECK_NEAR(actual, expected, tolerance) \
    ::bafx::test::checkNear((actual), (expected), (tolerance))

