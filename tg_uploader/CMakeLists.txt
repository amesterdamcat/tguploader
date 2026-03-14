cmake_minimum_required(VERSION 3.14)
project(tg_uploader CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# TDLib (built at /root/td, installed to /root/td/tdlib)
find_package(Td REQUIRED PATHS /root/td/tdlib/lib/cmake/Td)

# FFmpeg via pkg-config
find_package(PkgConfig REQUIRED)
pkg_check_modules(AVFORMAT REQUIRED IMPORTED_TARGET libavformat)
pkg_check_modules(AVCODEC REQUIRED IMPORTED_TARGET libavcodec)
pkg_check_modules(AVUTIL REQUIRED IMPORTED_TARGET libavutil)

# nlohmann/json (system package)
find_package(nlohmann_json REQUIRED)

# libcurl for Bot API HTTP requests
find_package(CURL REQUIRED)

add_executable(tg_uploader
    src/main.cpp
    src/config.cpp
    src/td_client.cpp
    src/video_info.cpp
    src/caption.cpp
    src/folder_lock.cpp
    src/uploader.cpp
    src/utils.cpp
    src/scanner.cpp
    src/bot_client.cpp
    src/bot_uploader.cpp
)

find_package(SQLite3 REQUIRED)

target_link_libraries(tg_uploader PRIVATE
    Td::TdStatic
    PkgConfig::AVFORMAT
    PkgConfig::AVCODEC
    PkgConfig::AVUTIL
    nlohmann_json::nlohmann_json
    SQLite::SQLite3
    CURL::libcurl
)
