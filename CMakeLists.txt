cmake_minimum_required(VERSION 3.20)
project(kivfs C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
add_compile_options(-Wall -Wextra)

# Vlákna (pthread) – používá je fronta, tedy celá společná knihovna
find_package(Threads REQUIRED)

# Struktura:
#   common/{proto,net,util}   common_lib – přenositelná (Linux i macOS)
#   server/core               server_core – kostra modulů (jen Linux)
#   server/<modul>/           programy kivfs-<modul>
#   client/, tools/           kivfs-client, kivfs-check
#   tests/unit, tests/scenarios
# Includy se píší s cestou od kořene knihovny: "proto/packet.h",
# "net/socket.h", "core/module.h", "db/backend.h".

# ---------------------------------------------------------------------
# Společná knihovna: formát na drátě, sockety, fronta, konfigurace
# ---------------------------------------------------------------------
add_library(common_lib
    common/proto/errors.c      common/proto/errors.h
    common/proto/protocol.c    common/proto/protocol.h
    common/proto/packet.c      common/proto/packet.h
    common/proto/hello.c       common/proto/hello.h
    common/proto/diag.c        common/proto/diag.h
                               common/proto/fsproto.h
    common/net/socket.c        common/net/socket.h
    common/net/channel.c       common/net/channel.h
    common/util/queue.c        common/util/queue.h
    common/util/config.c       common/util/config.h
)
target_include_directories(common_lib PUBLIC common)
target_link_libraries(common_lib PUBLIC Threads::Threads)

# ---------------------------------------------------------------------
# Kostra serverových modulů: reactor (epoll), fronta událostí,
# konfigurace modulu, společná kostra – jen Linux (cíl je Docker).
# ---------------------------------------------------------------------
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    add_library(server_core
        server/core/reactor.c  server/core/reactor.h
        server/core/events.c   server/core/events.h
        server/core/modconf.c  server/core/modconf.h
        server/core/module.c   server/core/module.h
    )
    target_include_directories(server_core PUBLIC server)
    target_link_libraries(server_core PUBLIC common_lib)
endif()

# Klient – přenositelný (Linux i macOS)
add_executable(kivfs-client client/main.c)
target_link_libraries(kivfs-client PRIVATE common_lib)

# ---------------------------------------------------------------------
# Testy (spuštění: ctest v build adresáři)
# ---------------------------------------------------------------------
enable_testing()
foreach(t protocol channel queue hello)
    add_executable(test_${t} tests/unit/test_${t}.c)
    target_link_libraries(test_${t} PRIVATE common_lib)
    add_test(NAME ${t} COMMAND test_${t})
endforeach()

if(TARGET server_core)
    foreach(t reactor reactor_out events config)
        add_executable(test_${t} tests/unit/test_${t}.c)
        target_link_libraries(test_${t} PRIVATE server_core)
        add_test(NAME ${t} COMMAND test_${t})
    endforeach()
    # test_config načítá šablony z conf/
    target_compile_definitions(test_config PRIVATE KIVFS_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
endif()

# ---------------------------------------------------------------------
# Serverové moduly (každý samostatný program nad server_core)
# a kontrola propojení clusteru – jen Linux.
# ---------------------------------------------------------------------
if(TARGET server_core)
    foreach(m auth sync vfs db fs)
        add_executable(kivfs-${m} server/${m}/main.c)
        target_link_libraries(kivfs-${m} PRIVATE server_core)
    endforeach()

    add_executable(kivfs-check tools/check.c)
    target_link_libraries(kivfs-check PRIVATE server_core)

    # Scénář: 3 uzly na jednom stroji, propojení, výpadek a obnova Syncu
    add_test(NAME cluster_local
             COMMAND ${CMAKE_SOURCE_DIR}/tests/scenarios/cluster-local.sh
                     ${CMAKE_BINARY_DIR} ${CMAKE_SOURCE_DIR}/conf/local)
    set_tests_properties(cluster_local PROPERTIES
        TIMEOUT 120
        RUN_SERIAL TRUE)   # pevné porty 310xx–330xx
endif()
