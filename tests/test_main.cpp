#include "test_support.hpp"

#include <exception>
#include <iostream>
#include <vector>

namespace bafx::test
{

std::vector<TestCase>& registry()
{
    static std::vector<TestCase> tests;
    return tests;
}

}

int main()
{
    int failures = 0;
    for (const bafx::test::TestCase& test : bafx::test::registry())
    {
        try
        {
            test.function();
            std::cout << "[PASS] " << test.name << '\n';
        }
        catch (const std::exception& error)
        {
            ++failures;
            std::cerr << "[FAIL] " << test.name << ": " << error.what() << '\n';
        }
        catch (...)
        {
            ++failures;
            std::cerr << "[FAIL] " << test.name << ": unknown exception\n";
        }
    }

    std::cout << bafx::test::registry().size() << " tests, " << failures << " failures\n";
    return failures == 0 ? 0 : 1;
}

