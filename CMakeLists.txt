cmake_minimum_required(VERSION 3.20)
project(kivfs C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
add_compile_options(-Wall -Wextra)

# Vlákna (pthread) – používá je fronta, tedy celá společná knihovna
find_package(Threads REQUIRED)

# Společná knihovna
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
)
target_include_directories(common_lib PUBLIC common)
target_link_libraries(common_lib PUBLIC Threads::Threads)

# Reactor (epoll) – jen Linux. Na macOS se server ani jeho testy
# nepřekládají; cílové prostředí je Docker s Linuxem.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    add_library(reactor_lib
        server/reactor.c
        server/reactor.h
    )
    target_include_directories(reactor_lib PUBLIC server)
    target_link_libraries(reactor_lib PUBLIC common_lib)
endif()

# Server – potřebuje reactor (epoll), takže jen Linux. Na macOS se
# překládá a spouští v Dockeru.
if(TARGET reactor_lib)
    add_executable(kivfs-server
        server/main.c
    )
    target_link_libraries(kivfs-server PRIVATE reactor_lib)
endif()

# Klient – přenositelný (Linux i macOS)
add_executable(kivfs-client
    client/main.c
)
target_link_libraries(kivfs-client PRIVATE common_lib)

# Testy (spuštění: ctest v build adresáři)
enable_testing()
add_executable(test_protocol
    tests/test_protocol.c
)
target_link_libraries(test_protocol PRIVATE common_lib)
add_test(NAME protocol COMMAND test_protocol)

add_executable(test_channel
    tests/test_channel.c
)
target_link_libraries(test_channel PRIVATE common_lib)
add_test(NAME channel COMMAND test_channel)

add_executable(test_queue
    tests/test_queue.c
)
target_link_libraries(test_queue PRIVATE common_lib)
add_test(NAME queue COMMAND test_queue)

if(TARGET reactor_lib)
    add_executable(test_reactor
        tests/test_reactor.c
    )
    target_link_libraries(test_reactor PRIVATE reactor_lib)
    add_test(NAME reactor COMMAND test_reactor)
endif()
