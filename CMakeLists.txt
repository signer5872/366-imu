cmake_minimum_required(VERSION 3.15)
project(imu_converter VERSION 1.0.0 LANGUAGES C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

add_library(imu SHARED src/imu_converter.c)
target_compile_definitions(imu PRIVATE IMU_BUILD)
target_include_directories(imu
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)
set_target_properties(imu PROPERTIES
    OUTPUT_NAME imu
    VERSION ${PROJECT_VERSION}
    SOVERSION 1
    C_VISIBILITY_PRESET hidden
)

add_executable(imu_convert main.c)
target_link_libraries(imu_convert PRIVATE imu)

if(MSVC)
    target_compile_options(imu PRIVATE /W4)
    target_compile_options(imu_convert PRIVATE /W4)
else()
    target_compile_options(imu PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_options(imu_convert PRIVATE -Wall -Wextra -Wpedantic)
endif()

include(CTest)
if(BUILD_TESTING)
    add_executable(imu_converter_test tests/test_converter.c)
    target_link_libraries(imu_converter_test PRIVATE imu)
    if(NOT MSVC)
        target_compile_options(imu_converter_test PRIVATE -Wall -Wextra -Wpedantic)
    endif()
    add_test(NAME imu_converter_test COMMAND imu_converter_test)
endif()

include(GNUInstallDirs)
install(TARGETS imu imu_convert
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
)
install(FILES include/imu_converter.h DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
