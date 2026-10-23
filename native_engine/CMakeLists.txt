cmake_minimum_required(VERSION 3.20)
project(maf_native_engine LANGUAGES CXX)

add_executable(maf_native_engine
    main.cpp
)

target_compile_features(maf_native_engine PRIVATE cxx_std_20)
target_compile_definitions(maf_native_engine PRIVATE UNICODE _UNICODE NOMINMAX)
target_link_libraries(maf_native_engine PRIVATE
    advapi32
    avrt
    ole32
    uuid
)
