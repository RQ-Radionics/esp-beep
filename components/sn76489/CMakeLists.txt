idf_component_register(
    SRCS "sn76489.c" "sn76489_audio.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_driver_dac
)
