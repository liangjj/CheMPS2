include_directories (${CheMPS2_SOURCE_DIR}/CheMPS2/include/)

link_directories (${CheMPS2_BINARY_DIR}/CheMPS2)

add_executable (test1 test1.cpp)
add_executable (test2 test2.cpp)
add_executable (test3 test3.cpp)
add_executable (test4 test4.cpp)
add_executable (test5 test5.cpp)
add_executable (test6 test6.cpp)

target_link_libraries (test1 CheMPS2)
target_link_libraries (test2 CheMPS2)
target_link_libraries (test3 CheMPS2)
target_link_libraries (test4 CheMPS2)
target_link_libraries (test5 CheMPS2)
target_link_libraries (test6 CheMPS2)

