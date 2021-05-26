#  Copyright (c) Microsoft Corporation. All rights reserved.
#  Licensed under the MIT License.

cmake_minimum_required (VERSION 3.10)

project (PlantMonitor C)

azsphere_configure_tools(TOOLS_REVISION "21.01")
azsphere_configure_api(TARGET_API_SET "9")

# Create executable
file(GLOB LIB_SRC lib/*/src/*.c)
add_executable (${PROJECT_NAME} main.c ${LIB_SRC})
target_include_directories(${PROJECT_NAME} PUBLIC ${AZURE_SPHERE_API_SET_DIR}/usr/include/azureiot 
                           ${AZURE_SPHERE_API_SET_DIR}/usr/include/azure_prov_client 
                           ${AZURE_SPHERE_API_SET_DIR}/usr/include/azure_c_shared_utility)
target_compile_definitions(${PROJECT_NAME} PUBLIC AZURE_IOT_HUB_CONFIGURED)
target_link_libraries(${PROJECT_NAME} m azureiot applibs gcc_s c)
azsphere_target_hardware_definition(${PROJECT_NAME} TARGET_DIRECTORY "HardwareDefinitions/avnet_mt3620_sk" TARGET_DEFINITION "plant_sk.json")

target_include_directories(${PROJECT_NAME} PRIVATE extern/C-Macro-Collections/src)
file(GLOB LIB_INC lib/*/inc)
target_include_directories(${PROJECT_NAME} PRIVATE ${LIB_INC})

azsphere_target_add_image_package(${PROJECT_NAME})
