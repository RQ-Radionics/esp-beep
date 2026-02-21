idf_component_register(
    SRCS "main.cpp"
    INCLUDE_DIRS "."
    REQUIRES esp_system bbc_machine sn76489 bbc_video nvs_flash esp_timer freertos fabgl
             esp_driver_sdspi sdmmc fatfs vfs
    EMBED_FILES
        ${CMAKE_SOURCE_DIR}/rom/os12.rom
        ${CMAKE_SOURCE_DIR}/rom/basic2.rom
)
