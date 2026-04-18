# Contributing to zcrudp

Thank you for your interest in contributing to `zcrudp`! We welcome contributions from the community to improve the protocol and its implementation.

## How to Report Bugs

If you find a bug or have a suggestion for improvement, please open an issue on the GitHub repository. When reporting a bug, please include:
- A clear and descriptive title.
- Steps to reproduce the issue.
- Expected behavior vs. actual behavior.
- Any relevant logs or error messages.

## Coding Standards

The project follows a strict set of coding standards to ensure consistency and maintainability. Please refer to [STYLE_CONVENTION.md](STYLE_CONVENTION.md) for detailed information on:
- C standards (C99/C11).
- Naming conventions (snake_case, suffixes).
- Formatting (Consistent with existing code, K&R style).
- Documentation (Doxygen-style).
- Memory management (Zero dynamic allocation).

## Test-Driven Development

We follow a test-driven development (TDD) approach. Any new features or bug fixes must include corresponding tests in the `tests/` directory.

To run the tests, use the provided `Makefile`:
```bash
make test
```
Binaries will be generated in the `build/` directory. Ensure all tests pass before submitting a Pull Request.

## Pull Request Process

1. **Fork the repository** and create your branch from `main`.
2. **Implement your changes** following the coding standards.
3. **Add tests** for your changes and ensure all existing tests pass.
4. **Update documentation** if necessary (e.g., `ARCHITECTURE.md`).
5. **Submit a Pull Request** with a clear description of your changes and why they are needed.

Your PR will be reviewed by the maintainers, and we may ask for changes before merging.

## Licensing

By contributing to `zcrudp`, you agree that your contributions will be licensed under the project's [LICENSE](LICENSE).
