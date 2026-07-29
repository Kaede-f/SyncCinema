# CMakeList.txt : CMake project for SyncCinema, include source and define
# project specific logic here.
#
cmake_minimum_required (VERSION 3.10)

# Enable Hot Reload for MSVC compilers if supported.
if (POLICY CMP0141)
  cmake_policy(SET CMP0141 NEW)
  set(CMAKE_MSVC_DEBUG_INFORMATION_FORMAT "$<IF:$<AND:$<C_COMPILER_ID:MSVC>,$<CXX_COMPILER_ID:MSVC>>,$<$<CONFIG:Debug,RelWithDebInfo>:EditAndContinue>,$<$<CONFIG:Debug,RelWithDebInfo>:ProgramDatabase>>")
endif()

project ("SyncCinema")

option(USE_LIBVLC "Use libVLC as the real local video player" OFF)

# libVLC is optional. Keep MockPlayer as the default so this learning project
# can compile even when VLC SDK is not installed.
#
# Example when enabling libVLC:
#   -DUSE_LIBVLC=ON
#   -DVLC_INCLUDE_DIR="C:/Program Files/VideoLAN/VLC/sdk/include"
#   -DVLC_LIBRARY="C:/Program Files/VideoLAN/VLC/sdk/lib/libvlc.lib"
#   -DVLC_CORE_LIBRARY="C:/Program Files/VideoLAN/VLC/sdk/lib/libvlccore.lib"
#   -DVLC_RUNTIME_DIR="C:/Program Files/VideoLAN/VLC"
set(VLC_INCLUDE_DIR "" CACHE PATH "Path to VLC SDK include directory, for example: C:/Program Files/VideoLAN/VLC/sdk/include")
set(VLC_LIBRARY "" CACHE FILEPATH "Path to libvlc.lib")
set(VLC_CORE_LIBRARY "" CACHE FILEPATH "Optional path to libvlccore.lib")
set(VLC_RUNTIME_DIR "" CACHE PATH "Optional VLC runtime directory containing libvlc.dll, libvlccore.dll, and plugins")

# Linux cloud deployment only needs the broadcast server. Keep this target small
# so client/player/libVLC code is not required to build the server.
add_executable(SyncCinemaServer
  "SyncServerMain.cpp"
  "Protocol.h"
  "Protocol.cpp"
  "Room.h"
  "Room.cpp"
  "SyncMetrics.h"
  "SyncMetrics.cpp"
  "SyncServer.h"
  "SyncServer.cpp"
  "NetSocket.h"
  "NetSocket.cpp"
)

if (WIN32)
  target_link_libraries(SyncCinemaServer PRIVATE ws2_32)

  # Add source to this project's original Windows executable.
  add_executable (SyncCinema
    "SyncCinema.cpp"
    "SyncCinema.h"
    "Protocol.h"
    "Protocol.cpp"
    "PlayerController.h"
    "PlayerController.cpp"
    "MockPlayer.h"
    "MockPlayer.cpp"
    "TcpServer.h"
    "TcpServer.cpp"
    "TcpClient.h"
    "TcpClient.cpp"
    "Room.h"
    "Room.cpp"
    "SyncMetrics.h"
    "SyncMetrics.cpp"
    "SyncServer.h"
    "SyncServer.cpp"
    "NetSocket.h"
    "NetSocket.cpp"
    "Client.h"
    "Client.cpp"
  )

  # Link Windows socket library.
  target_link_libraries(SyncCinema PRIVATE ws2_32)
endif()

if (USE_LIBVLC AND TARGET SyncCinema)
  if (NOT VLC_INCLUDE_DIR OR NOT VLC_LIBRARY)
    message(FATAL_ERROR "USE_LIBVLC is ON, but VLC_INCLUDE_DIR and VLC_LIBRARY are not configured.")
  endif()

  target_sources(SyncCinema PRIVATE
    "LibVlcPlayer.h"
    "LibVlcPlayer.cpp"
  )

  target_include_directories(SyncCinema PRIVATE "${VLC_INCLUDE_DIR}")
  target_link_libraries(SyncCinema PRIVATE "${VLC_LIBRARY}")

  if (VLC_CORE_LIBRARY)
    target_link_libraries(SyncCinema PRIVATE "${VLC_CORE_LIBRARY}")
  endif()

  target_compile_definitions(SyncCinema PRIVATE USE_LIBVLC)

  if (VLC_RUNTIME_DIR)
    add_custom_command(TARGET SyncCinema POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${VLC_RUNTIME_DIR}/libvlc.dll"
        "${VLC_RUNTIME_DIR}/libvlccore.dll"
        "$<TARGET_FILE_DIR:SyncCinema>"
      COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${VLC_RUNTIME_DIR}/plugins"
        "$<TARGET_FILE_DIR:SyncCinema>/plugins"
      COMMENT "Copying VLC runtime files next to SyncCinema.exe"
    )
  endif()
endif()

if (MSVC)
  target_compile_options(SyncCinemaServer PRIVATE /utf-8)
  if (TARGET SyncCinema)
    target_compile_options(SyncCinema PRIVATE /utf-8)
  endif()
endif()

if (CMAKE_VERSION VERSION_GREATER 3.12)
  set_property(TARGET SyncCinemaServer PROPERTY CXX_STANDARD 20)
  if (TARGET SyncCinema)
    set_property(TARGET SyncCinema PROPERTY CXX_STANDARD 20)
  endif()
endif()

# Cross-platform protocol and Room regression tests.
include(CTest)

if (BUILD_TESTING)
  find_package(Threads REQUIRED)

  # 核心协议/房间测试不依赖 libVLC，也不创建真实网络连接。
  # 因此 Windows 和 Linux 都能快速运行，用来锁住跨平台协议行为。
  add_executable(SyncCinemaCoreTests
    "tests/SyncCinemaCoreTests.cpp"
    "Protocol.h"
    "Protocol.cpp"
    "Room.h"
    "Room.cpp"
    "NetSocket.h"
    "NetSocket.cpp"
  )

  target_include_directories(SyncCinemaCoreTests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
  target_link_libraries(SyncCinemaCoreTests PRIVATE Threads::Threads)

  if (WIN32)
    target_link_libraries(SyncCinemaCoreTests PRIVATE ws2_32)
  endif()

  if (MSVC)
    target_compile_options(SyncCinemaCoreTests PRIVATE /utf-8)
  endif()

  if (CMAKE_VERSION VERSION_GREATER 3.12)
    set_property(TARGET SyncCinemaCoreTests PROPERTY CXX_STANDARD 20)
  endif()

  add_test(NAME SyncCinemaCoreTests COMMAND SyncCinemaCoreTests)
endif()
