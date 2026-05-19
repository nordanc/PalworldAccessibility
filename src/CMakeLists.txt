cmake_minimum_required(VERSION 3.22)

set(TARGET PalAccess)
project(${TARGET})

add_library(${TARGET} SHARED
    dllmain.cpp
    TolkBridge.cpp
    Speech.cpp
    Hooks.cpp
    Hotkeys.cpp
    Nav.cpp
)

target_include_directories(${TARGET} PRIVATE .)
target_link_libraries(${TARGET} PUBLIC UE4SS)

# Match UE4SS's C++23 requirement to avoid header-level incompatibilities.
set_target_properties(${TARGET} PROPERTIES
    CXX_STANDARD 23
    CXX_STANDARD_REQUIRED ON
    OUTPUT_NAME "main"   # UE4SS expects Mods/<ModName>/dlls/main.dll
)

# Optional: auto-deploy the built DLL into the Palworld game directory.
# Set -DPALWORLD_DIR=... at configure time to enable.
if(DEFINED PALWORLD_DIR AND EXISTS "${PALWORLD_DIR}/Pal/Binaries/Win64/Palworld-Win64-Shipping.exe")
    set(MOD_DST "${PALWORLD_DIR}/Pal/Binaries/Win64/Mods/PalAccessibility/dlls/main.dll")
    add_custom_command(TARGET ${TARGET} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory "${PALWORLD_DIR}/Pal/Binaries/Win64/Mods/PalAccessibility/dlls"
        COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:${TARGET}> "${MOD_DST}"
        COMMENT "Deploying PalAccess mod DLL to ${MOD_DST}"
    )
endif()
