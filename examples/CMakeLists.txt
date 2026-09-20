add_executable(gpplus_basic basic.cpp)
target_link_libraries(gpplus_basic PRIVATE gpplus::gpplus)

add_executable(gpplus_probe probe.cpp)
target_link_libraries(gpplus_probe PRIVATE gpplus::gpplus)

add_executable(gpplus_rebind rebind.cpp)
target_link_libraries(gpplus_rebind PRIVATE gpplus::gpplus)

# rebind writes its result with std::fopen, which MSVC's C4996 nags about;
# the library does the same for getenv (see the top-level CMakeLists.txt).
if(MSVC)
  target_compile_definitions(gpplus_rebind PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()
