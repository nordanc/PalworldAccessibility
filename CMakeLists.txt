cmake_minimum_required(VERSION 3.22)

# Top-level CMake. Mirrors the Dekita / UML "barebones C++ mod" layout:
#  - third_party/RE-UE4SS provides the `ue4ss` target
#  - PalAccess produces the mod DLL
project(PalworldAccessibility)

# UE4SS must be cloned at third_party/RE-UE4SS before configuring (see BUILD.md).
set(UE4SS_DIR "${CMAKE_SOURCE_DIR}/third_party/RE-UE4SS")
if(EXISTS "${UE4SS_DIR}/CMakeLists.txt")
    add_subdirectory("${UE4SS_DIR}" RE-UE4SS)
else()
    message(FATAL_ERROR
        "RE-UE4SS not found at ${UE4SS_DIR}. "
        "Run scripts/fetch-ue4ss.bat or clone manually: "
        "git clone --recurse-submodules https://github.com/UE4SS-RE/RE-UE4SS "
        "third_party/RE-UE4SS")
endif()

# UE4SS's top-level CMakeLists adds many internal targets. Our mod only
# needs to link against the public `UE4SS` library target.

add_subdirectory(src)
