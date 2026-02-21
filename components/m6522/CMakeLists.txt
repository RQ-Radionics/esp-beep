idf_component_register(
    SRCS "m6522.c" "bbc_sysvia.c" "bbc_uservia.c"
    INCLUDE_DIRS "include"
    REQUIRES "wd1770"
)
