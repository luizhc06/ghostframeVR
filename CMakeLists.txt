cmake_minimum_required(VERSION 3.16)
project(ghostframe CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_VISIBILITY_PRESET hidden)

# Nome final da DLL. Precisa ser IGUAL ao nome da DLL do driver da Pico
# (descobrir na pasta de instalação do Pico Connect). Ex.: -DGF_DRIVER_NAME=driver_pico
set(GF_DRIVER_NAME "driver_ghostframe" CACHE STRING "Nome da DLL do driver da Pico (sem extensão)")

add_library(ghostframe_proxy SHARED
    src/proxy/driver_main.cpp
    src/proxy/wrappers.cpp
    src/proxy/platform.cpp
    src/proxy/log.cpp
)
target_include_directories(ghostframe_proxy PRIVATE third_party/openvr)
set_target_properties(ghostframe_proxy PROPERTIES OUTPUT_NAME ${GF_DRIVER_NAME} PREFIX "")

if(MSVC)
    target_compile_options(ghostframe_proxy PRIVATE /W4 /permissive-)
    # CRT estático: não depende de redistribuível instalado no PC.
    set_property(TARGET ghostframe_proxy PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
elseif(WIN32)
    # MinGW
    target_link_options(ghostframe_proxy PRIVATE -static -static-libgcc -static-libstdc++)
    target_compile_options(ghostframe_proxy PRIVATE -Wall -Wextra)
else()
    target_compile_options(ghostframe_proxy PRIVATE -Wall -Wextra)
endif()

# ---- Testes (só fora do Windows: simulam SteamVR + driver Pico) ----
if(NOT WIN32)
    enable_testing()
    add_library(fake_pico_real SHARED tests/fake_pico_driver.cpp)
    target_include_directories(fake_pico_real PRIVATE third_party/openvr)
    # O proxy procura "<nome>_real.so" ao lado dele mesmo.
    set_target_properties(fake_pico_real PROPERTIES OUTPUT_NAME ${GF_DRIVER_NAME}_real PREFIX "")

    add_executable(fake_vrserver tests/fake_vrserver.cpp)
    target_include_directories(fake_vrserver PRIVATE third_party/openvr)
    target_link_libraries(fake_vrserver PRIVATE ${CMAKE_DL_LIBS})

    add_test(NAME proxy_passthrough
        COMMAND fake_vrserver $<TARGET_FILE:ghostframe_proxy> $<TARGET_FILE:fake_pico_real>)
endif()
