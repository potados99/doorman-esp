#!/bin/bash

# 프로젝트 루트에서 실행해야 해요!

cmake --build cmake-build-host-test && ./cmake-build-host-test/host_test_executable
