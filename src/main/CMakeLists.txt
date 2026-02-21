idf_component_register(
    SRCS "main.cpp"
    INCLUDE_DIRS "."
    REQUIRES esp32 bbc_machine sn76489 bbc_video nvs_flash esp_timer freertos
    COMPONENT_EMBED_TXTFILES 
        ${CMAKE_SOURCE_DIR}/rom/os12.rom
        ${CMAKE_SOURCE_DIR}/rom/basic2.rom
)
