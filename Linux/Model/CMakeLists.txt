cmake_minimum_required(VERSION 3.14)
project(FaceRecognition VERSION 1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ---- ncnn ----
if(NOT DEFINED NCNN_DIR)
    if(WIN32)
        set(NCNN_DIR "${CMAKE_CURRENT_SOURCE_DIR}/ncnn-tools/ncnn-20241226-windows-vs2022/x64")
    else()
        set(NCNN_DIR "/usr/local")
    endif()
endif()

set(NCNN_INCLUDE_DIR "${NCNN_DIR}/include")
if(WIN32)
    find_library(NCNN_LIB ncnn PATHS "${NCNN_DIR}/lib" NO_DEFAULT_PATH REQUIRED)
    set(NCNN_LIBS ${NCNN_LIB}
        "${NCNN_DIR}/lib/glslang.lib"
        "${NCNN_DIR}/lib/OSDependent.lib"
        "${NCNN_DIR}/lib/OGLCompiler.lib"
        "${NCNN_DIR}/lib/SPIRV.lib"
        "${NCNN_DIR}/lib/MachineIndependent.lib"
        "${NCNN_DIR}/lib/GenericCodeGen.lib"
    )
else()
    find_library(NCNN_LIB ncnn PATHS "${NCNN_DIR}/lib" NO_DEFAULT_PATH REQUIRED)
    set(NCNN_LIBS ${NCNN_LIB})
endif()
include_directories("${NCNN_INCLUDE_DIR}")
include_directories("${NCNN_DIR}/include/glslang")

# ---- OpenCV ----
find_package(OpenCV REQUIRED)

# ---- Sources ----
set(SOURCES
    src/facedetector.cpp
    src/facealigner.cpp
    src/featureextractor.cpp
    src/main.cpp
)
add_executable(facerec ${SOURCES})
target_include_directories(facerec PRIVATE "${CMAKE_SOURCE_DIR}/inc")
target_link_libraries(facerec PRIVATE ${OpenCV_LIBS} ${NCNN_LIBS})