cmake_minimum_required( VERSION 3.4.1 )
set( CMAKE_CXX_STANDARD 11 )

add_executable( libinjectvm.so src/main/jni/libinject/inject.cpp src/main/jni/inject_main.cpp )
add_library( payload SHARED src/main/jni/payload_main.cpp )

target_link_libraries( libinjectvm.so log dl )
target_link_libraries( payload log m )

target_compile_definitions( libinjectvm.so PRIVATE DEBUG )
target_compile_definitions( payload PRIVATE DEBUG )
