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
    common/proto/fsproto.c     common/proto/fsproto.h
    common/proto/syncproto.c   common/proto/syncproto.h
    common/net/socket.c        common/net/socket.h
    common/net/channel.c       common/net/channel.h
    common/util/queue.c        common/util/queue.h
    common/util/config.c       common/util/config.h
    common/util/sha256.c       common/util/sha256.h
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
foreach(t protocol channel queue hello fsproto syncproto sha256)
    add_executable(test_${t} tests/unit/test_${t}.c)
    target_link_libraries(test_${t} PRIVATE common_lib)
    add_test(NAME ${t} COMMAND test_${t})
endforeach()

# mock úložiště DB je čisté C bez sítě – test běží i na macOS
add_executable(test_db_mock tests/unit/test_db_mock.c server/db/backend_mock.c)
target_include_directories(test_db_mock PRIVATE server)
target_link_libraries(test_db_mock PRIVATE common_lib)
add_test(NAME db_mock COMMAND test_db_mock)

# úložiště bloků FS (adresář + mock) – jen POSIX, běží i na macOS
add_executable(test_store tests/unit/test_store.c
               server/fs/store_dir.c server/fs/store_mock.c)
target_include_directories(test_store PRIVATE server)
target_link_libraries(test_store PRIVATE common_lib)
add_test(NAME store COMMAND test_store)

# pruh Syncu (hodiny + seřazená fronta) – čistá datová struktura, přenositelné
add_executable(test_lane tests/unit/test_lane.c server/sync/lane.c)
target_include_directories(test_lane PRIVATE server)
target_link_libraries(test_lane PRIVATE common_lib)
add_test(NAME lane COMMAND test_lane)

# kontrola práv VFS – čisté funkce, také přenositelné
add_executable(test_vfs_perm tests/unit/test_vfs_perm.c server/vfs/perm.c)
target_include_directories(test_vfs_perm PRIVATE server)
target_link_libraries(test_vfs_perm PRIVATE common_lib)
add_test(NAME vfs_perm COMMAND test_vfs_perm)

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
    target_sources(kivfs-vfs PRIVATE server/vfs/perm.c         server/vfs/perm.h)
    target_sources(kivfs-db  PRIVATE server/db/backend_mock.c  server/db/backend.h)
    target_sources(kivfs-sync PRIVATE
        server/sync/sync.h    server/sync/lane.c    server/sync/lane.h
        server/sync/origin.c  server/sync/peer.c    server/sync/outbox.c
        server/sync/resolve.c server/sync/apply.c)

    add_executable(kivfs-check tools/check.c)
    target_link_libraries(kivfs-check PRIVATE server_core)

    # Scénář: 3 uzly na jednom stroji, propojení, výpadek a obnova Syncu
    add_test(NAME cluster_local
             COMMAND ${CMAKE_SOURCE_DIR}/tests/scenarios/cluster-local.sh
                     ${CMAKE_BINARY_DIR} ${CMAKE_SOURCE_DIR}/conf/local)
    set_tests_properties(cluster_local PROPERTIES
        TIMEOUT 120
        RUN_SERIAL TRUE)   # pevné porty 310xx–312xx

    # Scénář: operace FS přes klienta na jednom uzlu, výpadek a obnova DB
    add_test(NAME fs_local
             COMMAND ${CMAKE_SOURCE_DIR}/tests/scenarios/fs-local.sh
                     ${CMAKE_BINARY_DIR} ${CMAKE_SOURCE_DIR}/conf/single/node1.ini)
    set_tests_properties(fs_local PROPERTIES
        TIMEOUT 120
        RUN_SERIAL TRUE)

    # Scénář: kolo hlasování Syncu na 3 uzlech – souběh, výpadek, obnova,
    # zamrzlý uzel (op_timeout_ms zkrácen na 2 s v kopii konfigurace)
    add_test(NAME sync_local
             COMMAND ${CMAKE_SOURCE_DIR}/tests/scenarios/sync-local.sh
                     ${CMAKE_BINARY_DIR} ${CMAKE_SOURCE_DIR}/conf/local)
    set_tests_properties(sync_local PROPERTIES
        TIMEOUT 150
        RUN_SERIAL TRUE)   # pevné porty 310xx–312xx

    # Scénář: replikace metadat (pruh DB) na 3 uzlech – shoda stromu, souběh
    # stejných jmen, zamrzlá / restartovaná DB, restart Syncu s ověřením
    add_test(NAME repl_local
             COMMAND ${CMAKE_SOURCE_DIR}/tests/scenarios/repl-local.sh
                     ${CMAKE_BINARY_DIR} ${CMAKE_SOURCE_DIR}/conf/local)
    set_tests_properties(repl_local PROPERTIES
        TIMEOUT 150
        RUN_SERIAL TRUE)   # pevné porty 310xx–312xx
endif()
