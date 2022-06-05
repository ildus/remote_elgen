cmake_minimum_required(VERSION 3.5)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(remote_elgen)

set(COMPONENT_DIRS
	$ENV{IDF_PATH}/components
)

add_custom_target(idf_monitor
	COMMAND ${PYTHON} ${IDF_PATH}/tools/idf_monitor.py --port "/dev/ttyUSB0" ${IDF_PROJECT_EXECUTABLE}
    WORKING_DIRECTORY ${CMAKE_PROJECT_DIR}
)
