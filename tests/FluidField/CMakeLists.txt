# Fluid Field solver referee: runs the real WindSolver and HLSL on a WARP (software)
# D3D11 device and compares solver steps against a CPU reference.
#
# Standalone on purpose, so it needs no vcpkg and stays out of the plugin build:
#   cmake -S tests/FluidField -B build/FluidFieldTest
#   cmake --build build/FluidFieldTest --config Release
#   ctest --test-dir build/FluidFieldTest -C Release --output-on-failure
cmake_minimum_required(VERSION 3.21)
project(FluidFieldSolverTest LANGUAGES CXX)

set(REPO_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../..)

add_executable(
    FluidFieldSolverTest
    SolverTest.cpp
    ${REPO_ROOT}/src/Features/FluidField/WindSolver.cpp
)
target_compile_features(FluidFieldSolverTest PRIVATE cxx_std_23)
target_compile_options(FluidFieldSolverTest PRIVATE /W4 /permissive- /EHsc)
target_include_directories(FluidFieldSolverTest PRIVATE ${REPO_ROOT}/src/Features)
target_compile_definitions(
    FluidFieldSolverTest
    PRIVATE
        FLUIDFIELD_SHADER_DIR="${REPO_ROOT}/features/Ground Fog/Shaders"
        FLUIDFIELD_COMMON_SHADER_DIR="${REPO_ROOT}/features/Shallow Water/Shaders"
        NOMINMAX
        WIN32_LEAN_AND_MEAN
)
target_link_libraries(FluidFieldSolverTest PRIVATE d3d11 d3dcompiler windowsapp)

enable_testing()
add_test(NAME FluidFieldSolverTest COMMAND FluidFieldSolverTest)
