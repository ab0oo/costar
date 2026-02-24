
# Consider dependencies only in project.
set(CMAKE_DEPENDS_IN_PROJECT_ONLY OFF)

# The set of languages for which implicit dependencies are needed:
set(CMAKE_DEPENDS_LANGUAGES
  "ASM"
  )
# The set of files for implicit dependencies of each language:
set(CMAKE_DEPENDS_CHECK_ASM
  "/home/johgor/src/costar/idf/build/x509_crt_bundle.S" "/home/johgor/src/costar/idf/build/esp-idf/mbedtls/CMakeFiles/__idf_mbedtls.dir/__/__/x509_crt_bundle.S.obj"
  )
set(CMAKE_ASM_COMPILER_ID "GNU")

# Preprocessor definitions for this target.
set(CMAKE_TARGET_DEFINITIONS_ASM
  "ESP_PLATFORM"
  "ESP_PSA_ITS_AVAILABLE"
  "IDF_VER=\"v6.1-dev-2636-g97d9585357\""
  "MBEDTLS_CONFIG_FILE=\"mbedtls/esp_config.h\""
  "MBEDTLS_MAJOR_VERSION=4"
  "SOC_MMU_PAGE_SIZE=CONFIG_MMU_PAGE_SIZE"
  "SOC_XTAL_FREQ_MHZ=CONFIG_XTAL_FREQ"
  "TF_PSA_CRYPTO_USER_CONFIG_FILE=\"mbedtls/esp_config.h\""
  "_GLIBCXX_HAVE_POSIX_SEMAPHORE"
  "_GLIBCXX_USE_POSIX_SEMAPHORE"
  "_GNU_SOURCE"
  "_POSIX_READER_WRITER_LOCKS"
  )

# The include file search paths:
set(CMAKE_ASM_TARGET_INCLUDE_PATH
  "config"
  "/home/johgor/esp-idf/components/mbedtls/port/include"
  "/home/johgor/esp-idf/components/mbedtls/mbedtls/include"
  "/home/johgor/esp-idf/components/mbedtls/mbedtls/library"
  "/home/johgor/esp-idf/components/mbedtls/mbedtls/tf-psa-crypto/core"
  "/home/johgor/esp-idf/components/mbedtls/mbedtls/tf-psa-crypto/drivers/builtin/src"
  "/home/johgor/esp-idf/components/mbedtls/esp_crt_bundle/include"
  "/home/johgor/esp-idf/components/mbedtls/port/psa_driver/include"
  "/home/johgor/esp-idf/components/esp_libc/platform_include"
  "/home/johgor/esp-idf/components/freertos/config/include"
  "/home/johgor/esp-idf/components/freertos/config/include/freertos"
  "/home/johgor/esp-idf/components/freertos/config/xtensa/include"
  "/home/johgor/esp-idf/components/freertos/FreeRTOS-Kernel/include"
  "/home/johgor/esp-idf/components/freertos/FreeRTOS-Kernel/portable/xtensa/include"
  "/home/johgor/esp-idf/components/freertos/FreeRTOS-Kernel/portable/xtensa/include/freertos"
  "/home/johgor/esp-idf/components/freertos/esp_additions/include"
  "/home/johgor/esp-idf/components/esp_hw_support/include"
  "/home/johgor/esp-idf/components/esp_hw_support/include/soc"
  "/home/johgor/esp-idf/components/esp_hw_support/ldo/include"
  "/home/johgor/esp-idf/components/esp_hw_support/debug_probe/include"
  "/home/johgor/esp-idf/components/esp_hw_support/etm/include"
  "/home/johgor/esp-idf/components/esp_hw_support/mspi/mspi_timing_tuning/include"
  "/home/johgor/esp-idf/components/esp_hw_support/mspi/mspi_timing_tuning/tuning_scheme_impl/include"
  "/home/johgor/esp-idf/components/esp_hw_support/mspi/mspi_intr/include"
  "/home/johgor/esp-idf/components/esp_hw_support/power_supply/include"
  "/home/johgor/esp-idf/components/esp_hw_support/modem/include"
  "/home/johgor/esp-idf/components/esp_hw_support/port/esp32/."
  "/home/johgor/esp-idf/components/esp_hw_support/port/esp32/include"
  "/home/johgor/esp-idf/components/heap/include"
  "/home/johgor/esp-idf/components/heap/tlsf"
  "/home/johgor/esp-idf/components/log/include"
  "/home/johgor/esp-idf/components/soc/include"
  "/home/johgor/esp-idf/components/soc/esp32"
  "/home/johgor/esp-idf/components/soc/esp32/include"
  "/home/johgor/esp-idf/components/soc/esp32/register"
  "/home/johgor/esp-idf/components/hal/platform_port/include"
  "/home/johgor/esp-idf/components/hal/esp32/include"
  "/home/johgor/esp-idf/components/hal/include"
  "/home/johgor/esp-idf/components/esp_rom/include"
  "/home/johgor/esp-idf/components/esp_rom/esp32/include"
  "/home/johgor/esp-idf/components/esp_rom/esp32/include/esp32"
  "/home/johgor/esp-idf/components/esp_rom/esp32"
  "/home/johgor/esp-idf/components/esp_common/include"
  "/home/johgor/esp-idf/components/esp_system/include"
  "/home/johgor/esp-idf/components/esp_system/port/soc"
  "/home/johgor/esp-idf/components/esp_system/port/include/private"
  "/home/johgor/esp-idf/components/esp_stdio/include"
  "/home/johgor/esp-idf/components/xtensa/esp32/include"
  "/home/johgor/esp-idf/components/xtensa/include"
  "/home/johgor/esp-idf/components/xtensa/deprecated_include"
  "/home/johgor/esp-idf/components/esp_hal_gpio/include"
  "/home/johgor/esp-idf/components/esp_hal_gpio/esp32/include"
  "/home/johgor/esp-idf/components/esp_hal_usb/include"
  "/home/johgor/esp-idf/components/esp_hal_pmu/include"
  "/home/johgor/esp-idf/components/esp_hal_pmu/esp32/include"
  "/home/johgor/esp-idf/components/esp_hal_ana_conv/include"
  "/home/johgor/esp-idf/components/esp_hal_ana_conv/esp32/include"
  "/home/johgor/esp-idf/components/esp_hal_dma/include"
  "/home/johgor/esp-idf/components/esp_hal_i2s/include"
  "/home/johgor/esp-idf/components/esp_hal_i2s/esp32/include"
  "/home/johgor/esp-idf/components/lwip/include"
  "/home/johgor/esp-idf/components/lwip/include/apps"
  "/home/johgor/esp-idf/components/lwip/lwip/src/include"
  "/home/johgor/esp-idf/components/lwip/port/include"
  "/home/johgor/esp-idf/components/lwip/port/freertos/include"
  "/home/johgor/esp-idf/components/lwip/port/esp32xx/include"
  "/home/johgor/esp-idf/components/lwip/port/esp32xx/include/arch"
  "/home/johgor/esp-idf/components/lwip/port/esp32xx/include/sys"
  "/home/johgor/esp-idf/components/esp_security/include"
  "/home/johgor/esp-idf/components/esp_hal_security/esp32/include"
  "/home/johgor/esp-idf/components/esp_hal_security/include"
  "/home/johgor/esp-idf/components/esp_pm/include"
  "/home/johgor/esp-idf/components/esp_driver_dma/include"
  "/home/johgor/esp-idf/components/mbedtls/mbedtls/tf-psa-crypto/include"
  "/home/johgor/esp-idf/components/mbedtls/mbedtls/tf-psa-crypto/drivers/builtin/include"
  "/home/johgor/esp-idf/components/mbedtls/mbedtls/tf-psa-crypto/drivers/everest/include"
  "/home/johgor/esp-idf/components/mbedtls/mbedtls/tf-psa-crypto/drivers/p256-m"
  )

# The set of dependency files which are needed:
set(CMAKE_DEPENDS_DEPENDENCY_FILES
  "/home/johgor/esp-idf/components/mbedtls/esp_crt_bundle/esp_crt_bundle.c" "esp-idf/mbedtls/CMakeFiles/__idf_mbedtls.dir/esp_crt_bundle/esp_crt_bundle.c.obj" "gcc" "esp-idf/mbedtls/CMakeFiles/__idf_mbedtls.dir/esp_crt_bundle/esp_crt_bundle.c.obj.d"
  )

# Targets to which this target links which contain Fortran sources.
set(CMAKE_Fortran_TARGET_LINKED_INFO_FILES
  )

# Targets to which this target links which contain Fortran sources.
set(CMAKE_Fortran_TARGET_FORWARD_LINKED_INFO_FILES
  )

# Fortran module output directory.
set(CMAKE_Fortran_TARGET_MODULE_DIR "")
