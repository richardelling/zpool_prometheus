cmake_minimum_required(VERSION 3.10)
project(zpool_prometheus C)

# by default ZFSonLinux installs the header files and libraries in /usr/local
set(INSTALL_DIR /usr/local)
set(CMAKE_C_STANDARD 11)

include_directories(${INSTALL_DIR}/include/libspl ${INSTALL_DIR}/include/libzfs)
link_directories(${INSTALL_DIR}/lib)
add_executable(zpool_prometheus
        zpool_prometheus.c)
target_link_libraries(zpool_prometheus zfs nvpair)
install(TARGETS zpool_prometheus DESTINATION ${INSTALL_DIR}/bin)
