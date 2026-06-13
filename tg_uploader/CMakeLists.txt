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

# OpenSSL for HS256 JWT verification (used by the embedded web server)
find_package(OpenSSL REQUIRED)

# Threads for cpp-httplib's thread pool
find_package(Threads REQUIRED)

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
    src/web_server.cpp
    src/log_tee.cpp
    src/jwt_verify.cpp
    src/net_monitor.cpp
    src/user_auth.cpp
    src/job_history.cpp
    src/scheduler.cpp
    src/backup.cpp
    src/search_api.cpp
    src/recorder_api.cpp
    src/lequ_api.cpp
    src/lequ_sign.cpp
)

target_include_directories(tg_uploader PRIVATE include)

find_package(SQLite3 REQUIRED)

target_link_libraries(tg_uploader PRIVATE
    Td::TdStatic
    PkgConfig::AVFORMAT
    PkgConfig::AVCODEC
    PkgConfig::AVUTIL
    nlohmann_json::nlohmann_json
    SQLite::SQLite3
    CURL::libcurl
    OpenSSL::Crypto
    Threads::Threads
    crypt
)
