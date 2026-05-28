cmake_minimum_required(VERSION 3.16)
project(cyclomatic_complexity LANGUAGES C)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

add_compile_options(-O3)

find_package(PkgConfig REQUIRED)
pkg_check_modules(JSON_C REQUIRED IMPORTED_TARGET json-c)

# --- easy_pc Integration ---
include(FetchContent)

set(EASY_PC_LOCAL_PATH "" CACHE PATH "Path to local easy_pc repository")

if(DEFINED EASY_PC_LOCAL_PATH AND EXISTS ${EASY_PC_LOCAL_PATH})
  if(NOT IS_DIRECTORY ${EASY_PC_LOCAL_PATH})
    message(FATAL_ERROR "EASY_PC_LOCAL_PATH is not a valid path: ${EASY_PC_LOCAL_PATH}")
  endif()

  message(STATUS "Using local easy_pc repository at: ${EASY_PC_LOCAL_PATH}")
  add_subdirectory(${EASY_PC_LOCAL_PATH} easy_pc-build)
else()
  set(EASY_PC_GIT_REPO "git@github.com:ChrisNisbet01/easy_pc.git")
  set(EASY_PC_REPO_TAG "0.5.0")
  message(STATUS "Using github easy_pc repository at: ${EASY_PC_GIT_REPO}:${EASY_PC_REPO_TAG}")
  FetchContent_Declare(
    easy_pc
    GIT_REPOSITORY ${EASY_PC_GIT_REPO}
    GIT_TAG ${EASY_PC_REPO_TAG}
  )
  FetchContent_MakeAvailable(easy_pc)

  if(NOT EXISTS "${easy_pc_SOURCE_DIR}/Doxyfile")
    file(WRITE "${easy_pc_SOURCE_DIR}/Doxyfile" "# Dummy Doxyfile\nPROJECT_NAME = easy_pc\n")
  endif()
endif()

# Generate grammar from GDL
epc_generate_grammar(
    TARGET c_grammar_svr
    GDL_FILE ${CMAKE_CURRENT_SOURCE_DIR}/src/c_grammar.gdl
    HEADER callbacks.h
    BOOTSTRAP_AST
)

target_include_directories(c_grammar_svr PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)

add_executable(cyclomatic_complexity
    src/main.c
    src/complexity.c
    src/c_grammar_ast_actions.c
    src/ast_node_name.c
    src/debug.c
    src/source_location.c
    src/symbol_table.c
    src/generic_hash_table.c
)

target_compile_options(cyclomatic_complexity PRIVATE -O3 -g -std=gnu11 -Wall -Wextra -pedantic)

target_include_directories(cyclomatic_complexity PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_link_libraries(cyclomatic_complexity PRIVATE
    c_grammar_svr
    PkgConfig::JSON_C
)
