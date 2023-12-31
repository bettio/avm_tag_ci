#
# This file is part of AtomVM.
#
# Copyright 2022 Paul Guyot <pguyot@kallisys.net>
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#    http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
#
# SPDX-License-Identifier: Apache-2.0 OR LGPL-2.1-or-later
#

cmake_minimum_required (VERSION 3.13)

set(HEADER_FILES
    gpiodriver.h
    platform_defaultatoms.h
    platform_smp.h
    rp2040_sys.h
)

set(SOURCE_FILES
    gpiodriver.c
    platform_defaultatoms.c
    platform_nifs.c
    smp.c
    sys.c
)

set(
    PLATFORM_LIB_SUFFIX
    ${CMAKE_SYSTEM_NAME}-${CMAKE_SYSTEM_PROCESSOR}
)

add_library(libAtomVM${PLATFORM_LIB_SUFFIX} STATIC ${SOURCE_FILES} ${HEADER_FILES})
target_compile_features(libAtomVM${PLATFORM_LIB_SUFFIX} PUBLIC c_std_11)
if(CMAKE_COMPILER_IS_GNUCC)
    target_compile_options(libAtomVM${PLATFORM_LIB_SUFFIX} PRIVATE -Wall -pedantic -Wextra)
endif()

target_link_libraries(libAtomVM${PLATFORM_LIB_SUFFIX} PUBLIC libAtomVM)
target_link_libraries(
    libAtomVM${PLATFORM_LIB_SUFFIX}
    PUBLIC
        hardware_gpio
        hardware_rtc
        hardware_sync
        pico_float
        pico_multicore
        pico_platform
        pico_runtime
        pico_sync
        pico_time)

if (NOT AVM_USE_32BIT_FLOAT)
    target_link_libraries(libAtomVM${PLATFORM_LIB_SUFFIX} PUBLIC pico_double)
endif()

target_link_options(libAtomVM${PLATFORM_LIB_SUFFIX} PUBLIC -Wl,-u -Wl,gpio_nif)
