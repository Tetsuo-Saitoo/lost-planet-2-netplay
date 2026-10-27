cmake_minimum_required(VERSION 3.24)
project(lp2net VERSION 0.2.0 LANGUAGES CXX RC)

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
	message(FATAL_ERROR "Lost Planet 2 is a 32-bit game: configure with an x86 toolchain (vcvars32)")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# The one version number: build\generated\lp2net_version.h feeds the version resources, the log and the overlay.
set(LP2NET_GENERATED ${CMAKE_BINARY_DIR}/generated)
configure_file(src/version.h.in ${LP2NET_GENERATED}/lp2net_version.h @ONLY)

set(IMGUI_DIR ${CMAKE_SOURCE_DIR}/third_party/imgui)
add_library(imgui STATIC
	${IMGUI_DIR}/imgui.cpp
	${IMGUI_DIR}/imgui_draw.cpp
	${IMGUI_DIR}/imgui_tables.cpp
	${IMGUI_DIR}/imgui_widgets.cpp
	${IMGUI_DIR}/backends/imgui_impl_win32.cpp
	${IMGUI_DIR}/backends/imgui_impl_dx9.cpp
	${IMGUI_DIR}/backends/imgui_impl_dx11.cpp)
target_include_directories(imgui PUBLIC ${IMGUI_DIR} ${IMGUI_DIR}/backends)

add_library(xlive SHARED
	src/xlive/dllmain.cpp
	src/xlive/config.cpp
	src/xlive/log.cpp
	src/xlive/util.cpp
	src/xlive/xhandle.cpp
	src/xlive/xnotify.cpp
	src/xlive/xuser.cpp
	src/xlive/xsession.cpp
	src/xlive/xnet.cpp
	src/xlive/xsocket.cpp
	src/xlive/net.cpp
	src/xlive/upnp.cpp
	src/xlive/overlay.cpp
	src/xlive/forwards.cpp
	src/xlive/gamelog.cpp
	src/xlive/watchdog.cpp
	src/xlive/saves.cpp
	src/xlive/instances.cpp
	src/xlive/presence.cpp
	src/xlive/xui.cpp
	src/xlive/xstorage.cpp
	src/xlive/xstats.cpp
	src/xlive/steam.cpp
	src/xlive/matchmaking.cpp
	src/xlive/xlive.rc
	src/xlive/exports.def)
target_compile_definitions(xlive PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX _WINSOCK_DEPRECATED_NO_WARNINGS
	_CRT_SECURE_NO_WARNINGS)
target_compile_options(xlive PRIVATE $<$<COMPILE_LANGUAGE:CXX>:/W4 /wd4100 /utf-8>)
target_include_directories(xlive PRIVATE ${LP2NET_GENERATED})
target_link_libraries(xlive PRIVATE imgui ws2_32 iphlpapi bcrypt d3d9 d3d11 ole32 oleaut32)
set_target_properties(xlive PROPERTIES OUTPUT_NAME xlive)

# Two-process System Link transport test: run `nettest host`, then `nettest client` (prints PASS/FAIL).
add_executable(nettest tests/nettest.cpp)
target_compile_definitions(nettest PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
target_link_libraries(nettest PRIVATE ws2_32)
add_dependencies(nettest xlive)

# LP2NetSetup.exe: the players' installer. One file: xlive.dll is embedded as a resource; static CRT, so nothing
# else to install. (xlive.dll lands in the build folder with a single-config generator such as build.bat's Ninja.)
set(LP2NET_XLIVE_DLL ${CMAKE_BINARY_DIR}/xlive.dll)
file(CONFIGURE OUTPUT ${LP2NET_GENERATED}/payload.rc
	CONTENT "#include \"resource.h\"\nIDR_XLIVE RCDATA \"${LP2NET_XLIVE_DLL}\"\n" @ONLY)
add_executable(lp2netsetup WIN32
	src/setup/main.cpp
	src/setup/setup.cpp
	src/setup/firewall.cpp
	src/setup/setup.rc
	src/setup/setup.manifest
	${LP2NET_GENERATED}/payload.rc)
set_source_files_properties(${LP2NET_GENERATED}/payload.rc PROPERTIES OBJECT_DEPENDS ${LP2NET_XLIVE_DLL})
target_include_directories(lp2netsetup PRIVATE src/setup ${LP2NET_GENERATED})
target_compile_definitions(lp2netsetup PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
target_compile_options(lp2netsetup PRIVATE $<$<COMPILE_LANGUAGE:CXX>:/W4 /utf-8>)
target_link_libraries(lp2netsetup PRIVATE version bcrypt comctl32 ole32 oleaut32 shell32)
set_target_properties(lp2netsetup PROPERTIES OUTPUT_NAME LP2NetSetup)
add_dependencies(lp2netsetup xlive)
