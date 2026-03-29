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
    add_library(config
        components/config/config.cpp
    )
    target_include_directories(config PUBLIC
        components/config/include
    )

    add_library(statemachine
        components/statemachine/statemachine.cpp
    )
    target_include_directories(statemachine PUBLIC
        components/statemachine/include
    )
    target_link_libraries(statemachine PUBLIC config)

    # 테스트 바이너리
    add_executable(config_test
        host_test/config_test.cpp
    )
    target_link_libraries(config_test
        config
        GTest::gtest_main
    )

    add_executable(statemachine_test
        host_test/statemachine_test.cpp
    )
    target_link_libraries(statemachine_test
        statemachine
        GTest::gtest_main
    )

    include(GoogleTest)
    gtest_discover_tests(config_test)
    gtest_discover_tests(statemachine_test)
else()
    # ── ESP-IDF 빌드 ──
    include($ENV{IDF_PATH}/tools/cmake/project.cmake)
    project(doorman)
endif()
