// Host test entry point. The core suites always run; the variant suites are
// linked only into the full binary (SF_TEST_WEB / SF_TEST_HMI). The core-only
// binary is built with the core include path and core sources alone, which
// proves the shared core links without any web or HMI source.
#include <cstdio>

void runBridgeCoreTests();
void runControllerTests();
void runFtmsTests();
void runConfigStoreTests();
void runBleInputTests();
void runIfitBowflexTests();
void runTrainersRowerTests();
void runTreadmillTests();
void runSportstechTests();
void runAntTests();
#ifdef SF_TEST_WEB
void runWebTests();
#endif
#ifdef SF_TEST_HMI
void runHmiModelTests();
#endif

int main() {
    runBridgeCoreTests();
    runControllerTests();
    runFtmsTests();
    runConfigStoreTests();
    runBleInputTests();
    runIfitBowflexTests();
    runTrainersRowerTests();
    runTreadmillTests();
    runSportstechTests();
    runAntTests();
#ifdef SF_TEST_WEB
    runWebTests();
#endif
#ifdef SF_TEST_HMI
    runHmiModelTests();
#endif
#if defined(SF_TEST_WEB) || defined(SF_TEST_HMI)
    std::puts("host tests (core + variants) passed");
#else
    std::puts("host tests (core only) passed");
#endif
    return 0;
}
