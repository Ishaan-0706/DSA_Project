# Contributing to BlockDedup

Thanks for contributing! Please keep changes focused and explain the motivation
for non-obvious design decisions.

## Branches

Use `feature/<name>` for new functionality and `fix/<name>` for bug fixes.

## Build and test

```sh
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
```

## Formatting

Format C++ files with `clang-format` using the repository's `.clang-format`
configuration:

```sh
clang-format -i path/to/file.cpp path/to/file.hpp
```

## Pull requests

1. Create a focused branch and keep commits easy to review.
2. Run the build and test commands locally.
3. Open a pull request using the repository template.
4. Describe the change, link related issues, and address review feedback.
