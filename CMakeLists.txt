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

    # git HEAD가 바뀌면(커밋 후) CMake가 자동으로 reconfigure하여 버전 갱신
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
        "${CMAKE_SOURCE_DIR}/.git/HEAD"
        "${CMAKE_SOURCE_DIR}/.git/refs/heads/main"
    )

    # PROJECT_VER가 외부에서 안 주어지면 git revision count를 사용
    if(NOT DEFINED PROJECT_VER)
        execute_process(
            COMMAND git rev-list --count HEAD
            WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
            OUTPUT_VARIABLE PROJECT_VER
            OUTPUT_STRIP_TRAILING_WHITESPACE
            ERROR_QUIET
        )
        if(NOT PROJECT_VER)
            set(PROJECT_VER "0")
        endif()
    endif()

    include($ENV{IDF_PATH}/tools/cmake/project.cmake)
    set(PROJECT_VER "${PROJECT_VER}" CACHE STRING "Project version" FORCE)
    project(doorman)
endif()
