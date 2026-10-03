#include "minitest.h"

namespace minitest {

std::vector<Case>& registry()
{
    static std::vector<Case> r;
    return r;
}

int g_failures = 0;

} // namespace minitest

int main()
{
    int failedCases = 0;
    for (auto const& c : minitest::registry())
    {
        int before = minitest::g_failures;
        std::printf("[ RUN  ] %s\n", c.name);
        std::fflush(stdout);
        c.fn();
        bool ok = minitest::g_failures == before;
        failedCases += ok ? 0 : 1;
        std::printf("[ %s ] %s\n", ok ? " OK " : "FAIL", c.name);
    }
    std::printf("\n%zu cases, %d failed\n", minitest::registry().size(), failedCases);
    return failedCases == 0 ? 0 : 1;
}
