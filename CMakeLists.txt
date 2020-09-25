#  Copyright (c) Microsoft Corporation. All rights reserved.
#  Licensed under the MIT License.

cmake_minimum_required (VERSION 3.10)
project (MyBlink C)

azsphere_configure_tools(TOOLS_REVISION "20.07")
azsphere_configure_api(TARGET_API_SET "6")

# libuev
add_subdirectory(extern/libeuv)

# Create executable
add_executable (${PROJECT_NAME} pulse.c poll.c main.c lsm6dso_reg.c lps22hh_reg.c climatesensor.c soilmoisture.c humidity.c)
target_compile_definitions (${PROJECT_NAME} PUBLIC AZURE_IOT_HUB_CONFIGURED)
target_link_libraries (${PROJECT_NAME} m azureiot applibs pthread gcc_s c uev)
target_include_directories(${PROJECT_NAME} PRIVATE extern/C-Macro-Collections/src)

azsphere_target_hardware_definition(${PROJECT_NAME} TARGET_DIRECTORY "HardwareDefinitions/avnet_mt3620_sk" TARGET_DEFINITION "plant_sk.json")
azsphere_target_add_image_package(${PROJECT_NAME})