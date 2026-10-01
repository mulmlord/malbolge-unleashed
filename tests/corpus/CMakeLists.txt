# Classic conformance corpus (see README.md and SOURCES.md). One source, two manifests: the fast
# test runs manifest.txt; beer99 (13.8 million steps) is in manifest_slow.txt.
malm_add_test(NAME corpus_test LABEL fast SOURCES corpus_test.cpp LIBS malm_core TIMEOUT 120)
target_compile_definitions(corpus_test PRIVATE
    MALM_CORPUS_DIR="${CMAKE_CURRENT_SOURCE_DIR}" MALM_CORPUS_MANIFEST="manifest.txt")

malm_add_test(NAME corpus_slow_test LABEL slow SOURCES corpus_test.cpp LIBS malm_core
              PREFIX "slow:" TIMEOUT 600)
target_compile_definitions(corpus_slow_test PRIVATE
    MALM_CORPUS_DIR="${CMAKE_CURRENT_SOURCE_DIR}" MALM_CORPUS_MANIFEST="manifest_slow.txt")
