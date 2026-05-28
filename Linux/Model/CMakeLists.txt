cmake_minimum_required(VERSION 3.14)
project(FaceRecognition VERSION 1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ---- ncnn ----
# On Pi: ncnn installed to /usr/local
# Override with: cmake -DNCNN_DIR=/path/to/ncnn ..
if(NOT DEFINED NCNN_DIR)
    set(NCNN_DIR "/usr/local")
endif()

set(NCNN_INCLUDE_DIR "${NCNN_DIR}/include/ncnn")
find_library(NCNN_LIB ncnn PATHS "${NCNN_DIR}/lib" "${NCNN_DIR}/lib64" NO_DEFAULT_PATH REQUIRED)
include_directories("${NCNN_DIR}/include")

# ---- OpenCV ----
find_package(OpenCV REQUIRED)

# ---- OpenMP (ncnn depends on it) ----
find_package(OpenMP REQUIRED)

# ---- Sources ----
set(SOURCES
    src/facedetector.cpp
    src/facealigner.cpp
    src/featureextractor.cpp
    src/uart_protocol.cpp
    src/main.cpp
)

add_executable(facerec ${SOURCES})
target_include_directories(facerec PRIVATE "${CMAKE_SOURCE_DIR}/inc")
target_link_libraries(facerec PRIVATE ${OpenCV_LIBS} ${NCNN_LIB} OpenMP::OpenMP_CXX)
