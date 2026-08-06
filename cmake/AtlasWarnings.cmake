# Atlas - first-party warning policy.
# Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
#
# Warnings are treated as defects in Atlas' own code. They are never suppressed
# globally to make a build pass; if a warning is genuinely wrong for one
# expression, the fix is a local, commented cast.

function(atlas_set_warnings target)
  if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(${target} PRIVATE
      -Wall
      -Wextra
      -Wpedantic
      -Wconversion
      -Wshadow
      -Wformat=2
      -Wstrict-prototypes
      -Wmissing-prototypes
      -Wold-style-definition
      -Wwrite-strings
      -Wpointer-arith
      -Wcast-qual
      -Wundef
      -Wvla
      -Wswitch-enum
      -Wdouble-promotion
    )
    if(ATLAS_WERROR)
      target_compile_options(${target} PRIVATE -Werror)
    endif()
  endif()
endfunction()
