# Independent downstream consumer of the installed Microburst Governor package.
# Copyright 2026 Summon Software Labs.
# SPDX-License-Identifier: Apache-2.0
cmake_minimum_required(VERSION 3.25)
project(mbg_consumer LANGUAGES CXX)

find_package(mbg 1.0 CONFIG REQUIRED)

add_executable(mbg_consumer main.cpp)
target_link_libraries(mbg_consumer PRIVATE mbg::mbg)
target_compile_features(mbg_consumer PRIVATE cxx_std_20)
