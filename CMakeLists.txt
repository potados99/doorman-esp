cmake_minimum_required(VERSION 3.22)

if(HOST_TEST)
    # 순수 컴포넌트들의 호스트 테스트를 위한 셋업입니다.
    # 이 파일을 수정하면 CLion이 설정에 의해 cmake-build-host-test를 만들어줍니다.
    # 그러나 직접 만들려면 아래 명령을 쓰면 됩니다:
    #   cmake -DHOST_TEST=ON -S . -B cmake-build-host-test
    # 물론 CLion은 다른 옵션들도 같이 많이 넘겨주지만 결과는 똑같습니다.
    # 테스트 실행은 이렇게 하세요:
    #   cmake --build cmake-build-host-test && ./cmake-build-host-test/host_test_executable
    # 그냥 가서 빌드하는 명령입니다.

    # 프로젝트 선언은 맨 위에 합니다.
    # 아래 ESP 셋업은 맨 아래에 하는데 얘는 다른 점 주의..
    project(doorman_host_test CXX)

    # GoogleTest는 C++17 필요하다네요
    set(CMAKE_CXX_STANDARD 17)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)

    # GoogleTest 당겨오기!
    include(FetchContent)
    FetchContent_Declare(googletest GIT_REPOSITORY https://github.com/google/googletest.git GIT_TAG v1.17.0)
    FetchContent_MakeAvailable(googletest)

    # 저 밑에 gtest_discover_tests는 결국 CTest를 추가하는 동작이기 때문에 이게 필요해요.
    enable_testing()

    ################################ 테스트 추가할 때 바뀌는 섹션 ################################
    # 테스트를 추가하려면
    # 1. 먼저 컴포넌트 소스를 add_library, 헤더를 target_include_directories하고
    #    필요한 라이브러리를 target_link_libraries 해줍니다.
    # 2. 테스트 바이너리 add_executable에 테스트 코드(소스)를 추가해주고
    #    해당 컴포넌트를 테스트 바이너리에 target_link_libraries 해줍니다.

    # device 컴포넌트 — DeviceConfig 타입 + validate (순수, host_test 대상)
    add_library(device components/device/device.cpp)
    target_include_directories(device PUBLIC components/device/include)

    # statemachine 컴포넌트
    add_library(statemachine components/statemachine/statemachine.cpp)
    target_include_directories(statemachine PUBLIC components/statemachine/include)
    target_link_libraries(statemachine PUBLIC device)

    # 테스트 바이너리
    add_executable(host_test_executable
            # 여기 아래에 테스트 코드를 적어주면 됩니다.
            host_test/device_test.cpp
            host_test/statemachine_test.cpp
    )
    target_link_libraries(host_test_executable
            # 여기 아래에 add_library한 컴포넌트를 적어주면 됩니다.
            # 만약 라이브러리(=컴포넌트)가 다른 라이브러리를 링크하고 있다면
            # 위의 add_library 직후 이미 target_link_librariesf로 링크되었을 것이므로
            # 여기서 중복으로 링크하지 않아도 됩니다.
            statemachine
            GTest::gtest_main
    )

    include(GoogleTest)
    gtest_discover_tests(host_test_executable)
else()
    # 기기에 올릴 실제 펌웨어를 빌드하는 셋업입니다.
    # 이쪽도 마찬가지로 CLion이 cmake-build-esp-idf-debug를 만들어줍니다.
    # 이걸 직접 만들려면 이렇게 하면 됩니다:
    #   source ~/.espressif/tools/activate_idf_v6.0.sh
    #   cmake -S . -B cmake-build-esp-idf-debug
    # 그리고 빌드는 그냥 cmake --build 해도 되지만 이걸 추천합니다:
    #   idf.py -B ./cmake-build-esp-idf-debug build

    # 이 펌웨어에는 현재 git 리비전 넘버가 버전으로 들어갑니다.
    # 따라서 git HEAD가 바뀌면(커밋 후) CMake가 자동으로 reconfigure하여 버전을 갱신하도록 합니다.
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
        "${CMAKE_SOURCE_DIR}/.git/HEAD"
        "${CMAKE_SOURCE_DIR}/.git/refs/heads/main"
    )

    # git revision count를 꺼내와서 _GIT_REV_COUNT에 넣어둡니다.
    execute_process(
        COMMAND git rev-list --count HEAD
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        OUTPUT_VARIABLE _GIT_REV_COUNT
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
    )

    # 실패시 기본값은 0으로 맞춰두어요.
    if(NOT _GIT_REV_COUNT)
        set(_GIT_REV_COUNT "0")
    endif()

    # 그렇게 가져온 _GIT_REV_COUNT를 프로젝트 버전으로 씁니다.
    # 물론 -DPROJECT_VER=123 이런 식으로 빌드할 때 넣어줄 수도 있어요.
    set(PROJECT_VER "${_GIT_REV_COUNT}")

    # 여기가 본격적인 시작!
    include($ENV{IDF_PATH}/tools/cmake/project.cmake)

    # 하하 얘는 이게 맨 밑에 있어야 하네요.
    # 저 위에다가 두니까 configuring이 뭐 하지도 않고 끝남..
    project(doorman)
endif()
