# BlockDedup

BlockDedup is a C++20 project for exploring block-level duplicate and
near-duplicate detection. It provides the algorithmic building blocks for
content-defined chunking, hashing, similarity matching, and compact storage
deltas.

## Building

```sh
cmake -S . -B build
cmake --build build
```

## Testing

Tests use [Catch2](https://github.com/catchorg/Catch2), fetched automatically
by CMake:

```sh
ctest --test-dir build --output-on-failure
```

To configure without tests, pass `-DBUILD_TESTING=OFF` to the configure command.

## Contributing

See [CONTRIBUTING.md](CONTRIBUTING.md) for branch naming, formatting, testing,
and pull request guidance.

## License

BlockDedup is released under the [MIT License](LICENSE).
