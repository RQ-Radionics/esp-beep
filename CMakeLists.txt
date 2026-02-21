cmake_minimum_required(VERSION 3.16)

set(EXTRA_COMPONENT_DIRS 
    components/vrEmu6502/src
    components/bbc_cpu
    components/bbc_memory
    components/bbc_video
    components/bbc_machine
    components/fabgl
    components/wd1770
    components/m6522
    components/sn76489
)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(esp32-bbc)
