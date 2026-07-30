cmake_minimum_required(VERSION 3.15)
project(trading-core CXX)

# Require C++20
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(TRADING_CORE_LOWMEM "Low-RAM build: -O2, no LTO (for ~1GB VPS)" OFF)

# Compiler flags for performance and strict warnings
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|AppleClang|GNU")
    if(TRADING_CORE_LOWMEM)
        message(STATUS "TRADING_CORE_LOWMEM=ON: -O2, no LTO (VPS safe build)")
        set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -O2 -Wall -Wextra -Wpedantic -Wno-unused-parameter")
    else()
        set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -O3 -march=native -flto -Wall -Wextra -Wpedantic -Wno-unused-parameter")
    endif()
endif()

# Find dependencies provided by Conan
find_package(Boost REQUIRED COMPONENTS system thread json)
find_package(OpenSSL REQUIRED)
find_package(spdlog REQUIRED)

# Fetch libsecp256k1 for EIP-712 signing
include(FetchContent)
set(SECP256K1_ENABLE_MODULE_RECOVERY ON CACHE BOOL "" FORCE)
set(SECP256K1_ENABLE_MODULE_EXTRAKEYS ON CACHE BOOL "" FORCE)
set(SECP256K1_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(SECP256K1_BUILD_EXHAUSTIVE_TESTS OFF CACHE BOOL "" FORCE)
set(SECP256K1_BUILD_BENCHMARK OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
  secp256k1
  GIT_REPOSITORY https://github.com/bitcoin-core/secp256k1.git
  GIT_TAG        v0.7.1
)
FetchContent_MakeAvailable(secp256k1)

# Add source files
set(SOURCES
    src/main.cpp
    src/state/StateStore.cpp
    src/state/PaperStateStore.cpp
    src/feeds/BinanceFeed.cpp
    src/feeds/ChainlinkFeed.cpp
    src/feeds/PolymarketFeed.cpp
    src/feeds/GammaClient.cpp
    src/risk/RiskManager.cpp
    src/exec/EIP712Signer.cpp
    src/exec/OrderRouter.cpp
    src/signals/LegInHedgeDetector.cpp
    src/signals/RegimeGate.cpp
    src/telemetry/ShadowWindowRecorder.cpp
)

add_executable(trading-core ${SOURCES})

# Include directories
target_include_directories(trading-core PRIVATE src)

# Disable spdlog internal exceptions to bypass Apple Clang 15 C++20 consteval bugs
target_compile_definitions(trading-core PRIVATE SPDLOG_NO_EXCEPTIONS)

# Link dependencies
target_link_libraries(trading-core PRIVATE
    Boost::headers
    Boost::system
    Boost::thread
    Boost::json
    OpenSSL::SSL
    OpenSSL::Crypto
    spdlog::spdlog
    secp256k1
)
