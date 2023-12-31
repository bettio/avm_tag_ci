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

cmake_minimum_required(VERSION 3.13)

set(PICO_SDK_FETCH_FROM_GIT on)
set(CMAKE_TRY_COMPILE_TARGET_TYPE "STATIC_LIBRARY")

# pico_sdk_import.cmake is a single file copied from this SDK
# note: this must happen before project()
include(pico_sdk_import.cmake)

project(AtomVM)
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/../../../CMakeModules")

# initialize the Raspberry Pi Pico SDK
pico_sdk_init()

# Pico SDK forces compiler, but we really want to know its features.
include(${CMAKE_ROOT}/Modules/CMakeDetermineCompileFeatures.cmake)
CMAKE_DETERMINE_COMPILE_FEATURES(C)

enable_language( C CXX ASM )

# Pico SDK currently forces -nostdlib when using TRY_COMPILE
# This will make test of availability of POSIX functions fail but open and close
# are implemented to always return -1 anyway.
set(CMAKE_TRY_COMPILE_TARGET_TYPE EXECUTABLE)

# Options that make sense for this platform
option(AVM_DISABLE_SMP "Disable SMP support." OFF)
option(AVM_USE_32BIT_FLOAT "Use 32 bit floats." OFF)
option(AVM_VERBOSE_ABORT "Print module and line number on VM abort" OFF)
option(AVM_WAIT_FOR_USB_CONNECT "Wait for USB connection before starting" OFF)
option(AVM_REBOOT_ON_NOT_OK "Reboot Pico if result is not ok" OFF)

set(
    PLATFORM_LIB_SUFFIX
    ${CMAKE_SYSTEM_NAME}-${CMAKE_SYSTEM_PROCESSOR}
)

add_subdirectory(src)
add_subdirectory(tests)
