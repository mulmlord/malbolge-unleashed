# The VM benchmark (issue #37, docs/benchmarks.md); vm_bench.sh runs it on the benchmark programs.
add_executable(vm_bench vm_bench.cpp)
target_link_libraries(vm_bench PRIVATE malm_driver malm_warnings)

# Keeps the tool working; times nothing worth reading.
add_test(NAME vm_bench_smoke
    COMMAND vm_bench --min-time 0 ${PROJECT_SOURCE_DIR}/tests/programs/hello.mal)
set_tests_properties(vm_bench_smoke PROPERTIES LABELS fast TIMEOUT 60)

# The compression benchmark (issue #39, docs/benchmarks.md).
add_executable(codec_bench codec_bench.cpp)
target_link_libraries(codec_bench PRIVATE malm_driver malm_warnings)

add_test(NAME codec_bench_smoke
    COMMAND codec_bench --layout s8 --level 3 ${PROJECT_SOURCE_DIR}/tests/programs/hello.mal)
set_tests_properties(codec_bench_smoke PROPERTIES LABELS fast TIMEOUT 60)
