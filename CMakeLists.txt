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
    src/proxy/config.cpp
    src/proxy/framegen.cpp
    src/proxy/framegen_ref.cpp
)
target_include_directories(ghostframe_proxy PRIVATE third_party/openvr)
set_target_properties(ghostframe_proxy PROPERTIES OUTPUT_NAME ${GF_DRIVER_NAME} PREFIX "")
if(WIN32)
    # d3dcompiler_47.dll é carregada na hora pelo framegen.cpp (sem ela o proxy ainda carrega).
    target_link_libraries(ghostframe_proxy PRIVATE d3d11 dxgi d3dcompiler)
endif()

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

enable_testing()

# ---- Referência em CPU do frame gen (qualquer plataforma) ----
add_executable(framegen_ref_test tests/framegen_ref_test.cpp src/proxy/framegen_ref.cpp)
target_include_directories(framegen_ref_test PRIVATE src/proxy)
target_include_directories(framegen_ref_test SYSTEM PRIVATE third_party/openvr)
add_test(NAME framegen_ref COMMAND framegen_ref_test)
if(MSVC)
    target_compile_options(framegen_ref_test PRIVATE /W4 /permissive-)
else()
    target_compile_options(framegen_ref_test PRIVATE -Wall -Wextra)
endif()

if(WIN32)
    # ---- Motor do frame gen na GPU (WARP no CI, hardware se houver) ----
    add_executable(framegen_d3d_test
        tests/framegen_d3d_test.cpp
        src/proxy/framegen.cpp
        src/proxy/framegen_ref.cpp
        src/proxy/log.cpp
        src/proxy/platform.cpp
    )
    target_include_directories(framegen_d3d_test PRIVATE src/proxy)
    target_include_directories(framegen_d3d_test SYSTEM PRIVATE third_party/openvr)
    target_link_libraries(framegen_d3d_test PRIVATE d3d11 dxgi)
    if(MSVC)
        target_compile_options(framegen_d3d_test PRIVATE /W4 /permissive-)
    endif()
    add_test(NAME framegen_d3d COMMAND framegen_d3d_test)
endif()

# ---- Testes (só fora do Windows: simulam SteamVR + driver Pico) ----
if(NOT WIN32)
    add_library(fake_pico_real SHARED tests/fake_pico_driver.cpp)
    target_include_directories(fake_pico_real PRIVATE third_party/openvr)
    # O proxy procura "<nome>_real.so" ao lado dele mesmo.
    set_target_properties(fake_pico_real PROPERTIES OUTPUT_NAME ${GF_DRIVER_NAME}_real PREFIX "")

    add_executable(fake_vrserver tests/fake_vrserver.cpp)
    target_include_directories(fake_vrserver PRIVATE third_party/openvr)
    target_link_libraries(fake_vrserver PRIVATE ${CMAKE_DL_LIBS})

    add_test(NAME proxy_passthrough
        COMMAND fake_vrserver $<TARGET_FILE:ghostframe_proxy> $<TARGET_FILE:fake_pico_real>)
    # Frame gen ligado sem GPU (aqui o motor não inicia): tudo tem de continuar chegando ao driver.
    file(WRITE ${CMAKE_BINARY_DIR}/fg_ligado.ini "; teste\n[framegen]\nativo = 1\n")
    add_test(NAME proxy_framegen_sem_gpu
        COMMAND fake_vrserver $<TARGET_FILE:ghostframe_proxy> $<TARGET_FILE:fake_pico_real>)
    set_tests_properties(proxy_framegen_sem_gpu PROPERTIES ENVIRONMENT "GF_INI=${CMAKE_BINARY_DIR}/fg_ligado.ini")
endif()
