cmake_minimum_required(VERSION 3.26)

project(testbridge_package_consumer LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

find_package(TestBridge CONFIG REQUIRED)

add_executable(testbridge_package_consumer main.cpp)
target_link_libraries(testbridge_package_consumer PRIVATE TestBridge::testbridge)
