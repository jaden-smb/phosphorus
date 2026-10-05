// tests/main.cpp — the runner. Executes every PHX_TEST and reports.
#include "phx_test.h"

int main() {
    using namespace phxtest;
    std::printf("\nPhosphorus Engine — foundation test suite (%d cases)\n", count());
    std::printf("------------------------------------------------\n");
    for (int i = 0; i < count(); ++i) {
        std::printf("  • %s\n", storage()[i].name);
        storage()[i].fn();
    }
    std::printf("------------------------------------------------\n");
    if (failures() == 0) {
        std::printf("PASS  %d checks across %d cases\n\n", checks(), count());
        return 0;
    }
    std::printf("FAIL  %d of %d checks failed\n\n", failures(), checks());
    return 1;
}
