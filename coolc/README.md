# The COOL Programming Language Compiler

This directory and its subdirectories contain source code for **coolc**,
the compiler for **Classroom Object Oriented Language** created for educational purposes by **Alexander Aiken**.

Please see the documentation provided in **docs/** for further
assistance with **coolc**.

## Getting the Source Code and Building COOLC

1. Checkout COOLC:
    - `git clone https://github.com/xp10rd/Compilers.git`

2. Configure and build COOLC:
    - Prerequirement: **clang**, **gtest**, **llvm**;
    - `cd coolc`
    - `build.sh [-release/-debug] [options]`<br>
    Some usefull options:
        - `-clean` --- clean build directory before compile.
        - `-asan` --- build with AddressSanitizer (Debug only).
        - `-ubsan` --- build with UndefinedBehaviorSanitizer (Debug only).
        - `-test` --- run tests after building (Debug only).
        - `-mips` --- build for SPIM emulator.
        - `-llvm` --- build with **LLVM** for host architecture.