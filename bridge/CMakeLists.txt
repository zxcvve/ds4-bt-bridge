cmake_minimum_required(VERSION 3.20)
project(ds4bridge C CXX)

include(FetchContent)
set(BUILD_SHARED_LIBS OFF)
FetchContent_Declare(hidapi
    GIT_REPOSITORY https://github.com/libusb/hidapi
    GIT_TAG hidapi-0.15.0)
# master: the last tagged release predates vigem_target_ds4_await_output_report.
FetchContent_Declare(vigemclient
    GIT_REPOSITORY https://github.com/nefarius/ViGEmClient
    GIT_TAG b66d02d57e32cc8595369c53418b843e958649b4)
FetchContent_MakeAvailable(hidapi vigemclient)

add_executable(ds4bridge ds4bridge.c config.c imu.c stats.c sbc.c audio.c ds4_translate.c)
target_link_libraries(ds4bridge PRIVATE hidapi::winapi ViGEmClient::ViGEmClient setupapi cfgmgr32 ole32)
