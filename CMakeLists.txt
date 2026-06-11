cmake_minimum_required(VERSION 3.20)

cmake_policy(SET CMP0091 NEW)

project(OpenWithAssociatedApp LANGUAGES CXX)

add_library(OpenWithAssociatedApp SHARED
    OpenWithAssociatedApp.cpp
)

set(AVIUTL2_SDK_DIR "${CMAKE_CURRENT_LIST_DIR}/../aviutl2_sdk" CACHE PATH "Path to the AviUtl ExEdit2 plugin SDK headers")

if(NOT EXISTS "${AVIUTL2_SDK_DIR}/plugin2.h")
    message(FATAL_ERROR "plugin2.h not found in AVIUTL2_SDK_DIR (${AVIUTL2_SDK_DIR}). Set -DAVIUTL2_SDK_DIR=<path to aviutl2_sdk>.")
endif()

target_include_directories(OpenWithAssociatedApp PRIVATE
    "${AVIUTL2_SDK_DIR}"
)

target_compile_features(OpenWithAssociatedApp PRIVATE cxx_std_17)
target_compile_definitions(OpenWithAssociatedApp PRIVATE
    UNICODE
    _UNICODE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
)

set_target_properties(OpenWithAssociatedApp PROPERTIES
    PREFIX ""
    SUFFIX ".aux2"
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
)

if(MSVC)
    target_compile_options(OpenWithAssociatedApp PRIVATE /utf-8 /W4 /permissive- /EHsc /wd4819 /wd4828)
endif()

target_link_libraries(OpenWithAssociatedApp PRIVATE
    comctl32
    comdlg32
    ole32
    shell32
    user32
    gdi32
)
