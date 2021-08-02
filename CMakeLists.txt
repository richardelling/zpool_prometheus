cmake_minimum_required(VERSION 3.7)
project(zpool_prometheus C)

# by default ZFSonLinux installs the header files and libraries
# in /usr/local. If this is not the case for your system, set
# ZFS_INSTALL_BASE, e.g. for the Ubuntu zfs-on-linux library, use:
#     -D ZFS_INSTALL_BASE=/usr
# on the cmake command-line.
set(ZFS_INSTALL_BASE /usr/local CACHE STRING "zfs installation base directory")
set(CMAKE_INSTALL_PREFIX /usr/local)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -D_LARGEFILE64_SOURCE")

include_directories(${ZFS_INSTALL_BASE}/include/libspl ${ZFS_INSTALL_BASE}/include/libzfs)
link_directories(${ZFS_INSTALL_BASE}/lib)
add_executable(zpool_prometheus
        zpool_prometheus.c)
target_link_libraries(zpool_prometheus zfs nvpair)
install(TARGETS zpool_prometheus DESTINATION ${CMAKE_INSTALL_PREFIX}/bin)
