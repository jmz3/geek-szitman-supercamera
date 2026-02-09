cmake_minimum_required(VERSION 3.16)

project(geek_szitman_supercamera LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(Threads REQUIRED)
find_package(PkgConfig REQUIRED)

pkg_check_modules(LIBUSB QUIET IMPORTED_TARGET libusb-1.0)
if(NOT LIBUSB_FOUND)
    pkg_check_modules(LIBUSB REQUIRED IMPORTED_TARGET libusb)
endif()

find_package(OpenCV QUIET COMPONENTS highgui imgcodecs)
if(OpenCV_FOUND)
    set(USE_OPENCV_PACKAGE ON)
else()
    pkg_check_modules(OPENCV REQUIRED IMPORTED_TARGET opencv4)
    set(USE_OPENCV_PACKAGE OFF)
endif()

add_library(supercamera_core
    src/supercamera_core.cpp
)
target_include_directories(supercamera_core
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)
target_link_libraries(supercamera_core
    PUBLIC
        PkgConfig::LIBUSB
)
target_compile_features(supercamera_core PUBLIC cxx_std_23)

if(MSVC)
    target_compile_options(supercamera_core PRIVATE /W4)
else()
    target_compile_options(supercamera_core PRIVATE -Wall -Wextra)
endif()

add_executable(out
    src/supercamera_poc.cpp
)
target_link_libraries(out
    PRIVATE
        supercamera_core
        Threads::Threads
)

if(USE_OPENCV_PACKAGE)
    target_include_directories(out PRIVATE ${OpenCV_INCLUDE_DIRS})
    target_link_libraries(out PRIVATE ${OpenCV_LIBS})
else()
    target_link_libraries(out PRIVATE PkgConfig::OPENCV)
endif()

add_executable(out_stream_sender
    src/supercamera_stream_sender.cpp
)
target_link_libraries(out_stream_sender
    PRIVATE
        supercamera_core
        Threads::Threads
)
