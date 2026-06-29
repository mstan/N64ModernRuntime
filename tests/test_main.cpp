// Test entry point: runs every TEST() registered across the linked test
// translation units and prints a tally. Exit code is the number of failing
// cases (0 == all passed) so it can gate a build.
#include "test_framework.h"

int main() {
    int failed_cases = 0;
    int total_checks = 0;
    for (auto& c : n64mr_test::registry()) {
        n64mr_test::Result r;
        c.fn(r);
        total_checks += r.checks;
        if (r.failures != 0) {
            ++failed_cases;
            std::printf("[FAIL] %s (%d/%d checks failed)\n", c.name, r.failures, r.checks);
        } else {
            std::printf("[ ok ] %s (%d checks)\n", c.name, r.checks);
        }
    }
    std::printf("\n%zu cases, %d checks, %d cases failed\n",
                n64mr_test::registry().size(), total_checks, failed_cases);
    return failed_cases;
}
