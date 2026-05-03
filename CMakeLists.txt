cmake_minimum_required(VERSION 4.2)
project(shell C)

set(CMAKE_C_STANDARD 23)

add_executable(shell
        include/shell.h
        src/main.c
        src/lexer.c
        src/parser.c
        src/executor.c
        src/builtins.c
        src/history.c
        src/security.c
        include/plugin.h
        include/jobs.h
        src/jobs.c
        src/signals.c
        src/expand.c
        src/input.c
        include/input.h
        src/alias.c
        include/alias.h
        src/rc.c
        include/rc.h
        plugins/git.c
        src/plugin.c
        src/highlight.c
        include/highlight.h
        include/completions.h
        src/completions.c
        include/config.h
        src/config.c
        include/security.h)
