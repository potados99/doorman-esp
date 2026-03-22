cmake_minimum_required(VERSION 3.22)

if(HOST_TEST)
    # ── 호스트 테스트 빌드 (GTest) ──
    project(doorman_host_test CXX)

    set(CMAKE_CXX_STANDARD 17)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)

    include(FetchContent)
    FetchContent_Declare(
        googletest
        GIT_REPOSITORY https://github.com/google/googletest.git
        GIT_TAG v1.15.2
    )
    FetchContent_MakeAvailable(googletest)

    enable_testing()

    # 테스트 대상 컴포넌트
    add_library(gatekeeper
        components/gatekeeper/gatekeeper.cpp
    )
    target_include_directories(gatekeeper PUBLIC
        components/gatekeeper/include
    )

    # 테스트 바이너리
    add_executable(gatekeeper_test
        host_test/gatekeeper_test.cpp
    )
    target_link_libraries(gatekeeper_test
        gatekeeper
        GTest::gtest_main
    )

    include(GoogleTest)
    gtest_discover_tests(gatekeeper_test)
else()
    # ── ESP-IDF 빌드 ──
    include($ENV{IDF_PATH}/tools/cmake/project.cmake)
    project(doorman)
endif()
