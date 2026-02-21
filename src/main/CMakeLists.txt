idf_component_register(
    SRCS "main.cpp"
    INCLUDE_DIRS "."
    REQUIRES esp32 fabgl bbc_cpu bbc_memory nvs_flash esp_timer freertos
    COMPONENT_EMBED_TXTFILES 
        ${CMAKE_SOURCE_DIR}/rom/os12.rom
        ${CMAKE_SOURCE_DIR}/rom/basic2.rom
)
