include(FetchContent)

FetchContent_Declare(
    Catch2
    GIT_REPOSITORY https://github.com/catchorg/Catch2.git
    GIT_TAG v3.5.3
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(Catch2)

add_executable(test_rolling_hash test_rolling_hash.cpp)
target_link_libraries(test_rolling_hash PRIVATE blockdedup Catch2::Catch2WithMain)

include(Catch)
catch_discover_tests(test_rolling_hash)
