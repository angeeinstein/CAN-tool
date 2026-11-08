# The firmware is one ESP-IDF component ("main"): everything in src/ plus the platform-independent
# library lib/franke_core (SLCAN codec + rule engine, also used by the native unit tests).
file(GLOB app_sources ${CMAKE_CURRENT_LIST_DIR}/*.cpp)
file(GLOB core_sources ${CMAKE_CURRENT_LIST_DIR}/../lib/franke_core/src/*.cpp)

idf_component_register(
    SRCS ${app_sources} ${core_sources}
    INCLUDE_DIRS . ../include ../lib/franke_core/src
    REQUIRES driver esp_timer esp_wifi esp_netif esp_event esp_http_server app_update nvs_flash
             wpa_supplicant espcoredump esp_system esp_hw_support lwip mbedtls bootloader_support
             esp_app_format
)

target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra -Wno-unused-parameter)
