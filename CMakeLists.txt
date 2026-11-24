cmake_minimum_required(VERSION 3.20)
project(kivfs C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
add_compile_options(-Wall -Wextra)

# Vlákna (pthread) – používá je fronta, tedy celá společná knihovna
find_package(Threads REQUIRED)

# Společná knihovna: síťová vrstva (protokol, pakety, sockety, kanál)
# a obecná fronta pro předávání práce mezi vlákny.
add_library(common_lib
    common/errors.c
    common/errors.h
    common/protocol.c
    common/protocol.h
    common/packet.c
    common/packet.h
    common/socket.c
    common/socket.h
    common/channel.c
    common/channel.h
    common/queue.c
    common/queue.h
    common/config.c
    common/config.h
    common/hello.c
    common/hello.h
    common/diag.c
    common/diag.h
)
target_include_directories(common_lib PUBLIC common)
target_link_libraries(common_lib PUBLIC Threads::Threads)

# Reactor (epoll) pro příchozí i odchozí spojení + fronta událostí modulu – jen Linux. Na macOS se reactor
# ani jeho test nepřekládají; cílové prostředí je Docker s Linuxem.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    add_library(reactor_lib
        server/reactor.c
        server/reactor.h
        server/events.c
        server/events.h
        server/modconf.c
        server/modconf.h
        server/module.c
        server/module.h
    )
    target_include_directories(reactor_lib PUBLIC server)
    target_link_libraries(reactor_lib PUBLIC common_lib)
endif()

# Klient – přenositelný (Linux i macOS); zatím jen hello + ping
add_executable(kivfs-client
    client/main.c
)
target_link_libraries(kivfs-client PRIVATE common_lib)

# Testy (spuštění: ctest v build adresáři)
enable_testing()
foreach(t protocol channel queue hello)
    add_executable(test_${t} tests/test_${t}.c)
    target_link_libraries(test_${t} PRIVATE common_lib)
    add_test(NAME ${t} COMMAND test_${t})
endforeach()

if(TARGET reactor_lib)
    add_executable(test_reactor
        tests/test_reactor.c
    )
    target_link_libraries(test_reactor PRIVATE reactor_lib)
    add_test(NAME reactor COMMAND test_reactor)

    foreach(t reactor_out events config)
        add_executable(test_${t} tests/test_${t}.c)
        target_link_libraries(test_${t} PRIVATE reactor_lib)
        add_test(NAME ${t} COMMAND test_${t})
    endforeach()
    # test_config načítá šablony z conf/
    target_compile_definitions(test_config PRIVATE KIVFS_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
endif()

# Serverové moduly (každý samostatný program nad společnou kostrou
# server/module.c) a kontrola propojení clusteru – jen Linux.
if(TARGET reactor_lib)
    foreach(m auth sync vfs db fs)
        add_executable(kivfs-${m} server/main_${m}.c)
        target_link_libraries(kivfs-${m} PRIVATE reactor_lib)
    endforeach()

    add_executable(kivfs-check tools/check.c)
    target_link_libraries(kivfs-check PRIVATE reactor_lib)

    # Scénář: 3 uzly na jednom stroji, propojení, výpadek a obnova Syncu
    add_test(NAME cluster_local
             COMMAND ${CMAKE_SOURCE_DIR}/scripts/cluster-local.sh
                     ${CMAKE_BINARY_DIR} ${CMAKE_SOURCE_DIR}/conf/local)
    set_tests_properties(cluster_local PROPERTIES
        TIMEOUT 120
        RUN_SERIAL TRUE)   # pevné porty 310xx–330xx
endif()
