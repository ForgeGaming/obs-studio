project(graphics-hook)

set(COMPILE_D3D12_HOOK FALSE CACHE BOOL "Compile D3D12 hook support (required windows 10 SDK)")

if(CMAKE_SIZEOF_VOID_P EQUAL 8)
    set(_minhook_lib_name "libMinHook.x64.lib")
else()
    set(_minhook_lib_name "libMinHook.x86.lib")
endif()

set(USE_MINHOOK FALSE CACHE BOOL "Use MinHook")
set(MinHook_INCLUDE_DIRS "$ENV{MinHook}/include" CACHE PATH "MinHook include directory")
set(MinHook_LIBRARIES "$ENV{MinHook}/build/VC14/lib/Release/${_minhook_lib_name}" CACHE FILEPATH "MinHook library file")

configure_file(
	"${CMAKE_CURRENT_SOURCE_DIR}/graphics-hook-config.h.in"
	"${CMAKE_BINARY_DIR}/plugins/win-capture/graphics-hook/config/graphics-hook-config.h")


set(HOOK_INIT_LOG_PATH "" CACHE STRING "Set path for early hook initialization logs, e.g.: 'Forge/logs' will expand to '%LOCALAPPDATA%/Forge/logs/hook-init-log-%HOOKED_PID%.log'")
if (HOOK_INIT_LOG_PATH)
    add_definitions(-DHOOK_INIT_LOG_PATH="${HOOK_INIT_LOG_PATH}")
endif()


set(graphics-hook_HEADERS
	"${CMAKE_BINARY_DIR}/plugins/win-capture/graphics-hook/config/graphics-hook-config.h"
	graphics-hook.h
	../graphics-hook-info.h
	../hook-helpers.h
	../funchook.h
	../obfuscate.h
	gl-decs.h
	d3d9-patches.hpp)

set(graphics-hook_SOURCES
	graphics-hook.c
	../funchook.c
	../obfuscate.c
	gl-capture.c
	d3d8-capture.cpp
	d3d9-capture.cpp
	dxgi-capture.cpp
	d3d10-capture.cpp
	d3d11-capture.cpp
	d3d12-capture.cpp)

add_library(graphics-hook MODULE
	${graphics-hook_SOURCES}
	${graphics-hook_HEADERS})

target_include_directories(graphics-hook PUBLIC
	"${CMAKE_BINARY_DIR}/plugins/win-capture/graphics-hook/config")

if(USE_MINHOOK)
    target_include_directories(graphics-hook PUBLIC
        "${MinHook_INCLUDE_DIRS}")
    target_link_libraries(graphics-hook
        "${MinHook_LIBRARIES}")
endif()

target_link_libraries(graphics-hook
	ipc-util psapi)

if(CMAKE_SIZEOF_VOID_P EQUAL 8)
	set(_output_suffix "64")
else()
	set(_output_suffix "32")
endif()

set_target_properties(graphics-hook
	PROPERTIES
		OUTPUT_NAME "graphics-hook${_output_suffix}")

install_obs_datatarget(graphics-hook "obs-plugins/win-capture")
