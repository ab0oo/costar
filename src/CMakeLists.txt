idf_component_register(
  SRCS
    "../idf/main/app_main.cpp"
    "../idf/main/FsEspIdf.cpp"
    "../idf/main/NetEspIdf.cpp"
    "../idf/main/DisplayBootstrapEspIdf.cpp"
    "../idf/main/DisplaySpiEspIdf.cpp"
    "../idf/main/LayoutRuntimeEspIdf.cpp"
    "../idf/main/DslWidgetRuntimeEspIdf.cpp"
    "../idf/main/ConfigScreenEspIdf.cpp"
    "../idf/main/TouchInputEspIdf.cpp"
    "../idf/main/TextEntryEspIdf.cpp"
    "../idf/main/LvglPasswordPrompt.cpp"
    "../idf/main/PlatformEspIdf.cpp"
    "../idf/main/PrefsEspIdf.cpp"
    "core/BootCommon.cpp"
    "core/RuntimeSettings.cpp"
    "core/TimeSync.cpp"
  INCLUDE_DIRS
    "../idf/main"
    "../include"
    "."
  REQUIRES
    esp_event
    esp_timer
    esp_wifi
    esp_driver_gpio
    esp_driver_spi
    esp_http_client
    esp_websocket_client
    freertos
    heap
    littlefs
    mbedtls
    nvs_flash
    esp_netif
)

option(COSTAR_BUILD_LITTLEFS_IMAGE "Generate LittleFS image from data/ during build" ON)
if(COSTAR_BUILD_LITTLEFS_IMAGE)
  littlefs_create_partition_image(storage "../data" FLASH_IN_PROJECT)
else()
  message(WARNING "COSTAR_BUILD_LITTLEFS_IMAGE=OFF: skipping LittleFS image generation")
endif()
