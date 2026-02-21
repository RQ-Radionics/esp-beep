idf_component_register(
    SRCS "${CMAKE_CURRENT_SOURCE_DIR}/../../ext/vrEmu6502/src/vrEmu6502.c"
    INCLUDE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/../../ext/vrEmu6502/src"
)

# vrEmu6502 triggers -Werror=maybe-uninitialized with GCC 14; suppress it
target_compile_options(__idf_vrEmu6502 PRIVATE -Wno-maybe-uninitialized)
