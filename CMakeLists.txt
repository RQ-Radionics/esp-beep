cmake_minimum_required(VERSION 3.16)

set(EXTRA_COMPONENT_DIRS 
    components/vrEmu6502/src
    components/bbc_cpu
    components/bbc_memory
    components/bbc_video
    components/fabgl
)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(esp32-bbc)
