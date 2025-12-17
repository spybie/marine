cmake_minimum_required(VERSION 3.10)
project(battleship)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Для Windows
if(WIN32)
    add_definitions(-DWIN32)
    set(WS2_LIB ws2_32)
else()
    set(WS2_LIB "")
endif()

# Сервер
add_executable(battleship_server 
    battleship_server.cpp
    battleship.cpp
)
if(WIN32)
    target_link_libraries(battleship_server ${WS2_LIB})
endif()

# Клиент
add_executable(battleship_client 
    battleship_client.cpp
    battleship.cpp
)
if(WIN32)
    target_link_libraries(battleship_client ${WS2_LIB})
endif()

