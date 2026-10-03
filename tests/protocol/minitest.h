// Tiny self-contained test harness (no external dependency, builds for x86/x64).
#pragma once

#include <cstdio>
#include <functional>
#include <string>
#include <vector>

namespace minitest {

struct Case
{
    char const* name;
    std::function<void()> fn;
};

std::vector<Case>& registry();
extern int g_failures;

struct Registrar
{
    Registrar(char const* name, std::function<void()> fn) { registry().push_back({ name, std::move(fn) }); }
};

} // namespace minitest

#define MT_CONCAT2(a, b) a##b
#define MT_CONCAT(a, b) MT_CONCAT2(a, b)
#define TEST_CASE(name)                                                                        \
    static void MT_CONCAT(mt_fn_, __LINE__)();                                                 \
    static ::minitest::Registrar MT_CONCAT(mt_reg_, __LINE__)(name, &MT_CONCAT(mt_fn_, __LINE__)); \
    static void MT_CONCAT(mt_fn_, __LINE__)()

#define CHECK(cond)                                                                            \
    do                                                                                         \
    {                                                                                          \
        if (!(cond))                                                                           \
        {                                                                                      \
            ++::minitest::g_failures;                                                          \
            std::printf("    CHECK FAILED %s:%d: %s\n", __FILE__, __LINE__, #cond);            \
        }                                                                                      \
    } while (0)

#define CHECK_EQ(a, b)                                                                         \
    do                                                                                         \
    {                                                                                          \
        auto const& mt_a = (a);                                                                \
        auto const& mt_b = (b);                                                                \
        if (!(mt_a == mt_b))                                                                   \
        {                                                                                      \
            ++::minitest::g_failures;                                                          \
            std::printf("    CHECK_EQ FAILED %s:%d: %s == %s\n", __FILE__, __LINE__, #a, #b);  \
        }                                                                                      \
    } while (0)
