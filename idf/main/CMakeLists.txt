idf_component_register(
  SRCS
    "app_main.cpp"
    "FsEspIdf.cpp"
    "NetEspIdf.cpp"
    "DisplayBootstrapEspIdf.cpp"
    "DisplaySpiEspIdf.cpp"
    "ConfigScreenEspIdf.cpp"
    "TouchInputEspIdf.cpp"
    "PlatformEspIdf.cpp"
    "PrefsEspIdf.cpp"
    "../../src/core/BootCommon.cpp"
    "../../src/core/RuntimeSettings.cpp"
    "../../src/core/TimeSync.cpp"
  INCLUDE_DIRS
    "."
    "../../include"
    "../../src"
  REQUIRES
    esp_event
    esp_timer
    esp_wifi
    esp_driver_gpio
    esp_driver_spi
    freertos
    heap
    littlefs
    nvs_flash
    esp_netif
)

# Build and flash the LittleFS image for the "storage" partition from the project data directory.
# Disable when pip/network access is unavailable in the build environment.
option(COSTAR_BUILD_LITTLEFS_IMAGE "Generate LittleFS image from data/ during build" ON)
if(COSTAR_BUILD_LITTLEFS_IMAGE)
  littlefs_create_partition_image(storage "../../data" FLASH_IN_PROJECT)
else()
  message(WARNING "COSTAR_BUILD_LITTLEFS_IMAGE=OFF: skipping LittleFS image generation")
endif()
