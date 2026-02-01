# Magica Compiler

A systems programming language compiler written in C using LLVM as the sole backend.

## Features

- Systems Programming: Structs, enums, generics, loops, and user-defined types.
- Custom Pipeline: Hand-written lexer, parser, and type system.

## Requirements

- C Compiler (GCC/Clang)
- LLVM Toolchain (available in PATH or `~/.local/bin`)

## Build

Compiles the project in debug (default) or release mode:
```bash
# Debug
./build.sh

# Release
BUILD_TYPE=release ./build.sh
```
