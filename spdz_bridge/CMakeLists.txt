set(SPDZ_DIR ${CMAKE_SOURCE_DIR}/third_party/MP-SPDZ)

add_library(spdz_bridge 
    src/playersdata.cpp 
    src/spdz_paths.cpp 
    src/compile_program.cpp
    ${SPDZ_DIR}/Networking/sockets.cpp
    ${SPDZ_DIR}/Tools/Exceptions.cpp
    ${SPDZ_DIR}/Tools/octetStream.cpp
    ${SPDZ_DIR}/Tools/time-func.cpp
    ${SPDZ_DIR}/Math/bigint.cpp
)

target_include_directories(spdz_bridge PUBLIC ${SPDZ_DIR})