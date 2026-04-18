# Style Convention

This document outlines the coding standards and conventions used in the `zcrudp` project.

## C Standards

- The project follows the **C99** or **C11** standards.
- Use standard integer types from `<stdint.h>` (e.g., `uint32_t`, `uint16_t`).

## Naming Conventions

- **Functions**: Use `snake_case` (e.g., `rudp_init`, `rudp_send`).
- **Variables**: Use `snake_case` (e.g., `seq_num`, `ack_num`).
- **Structs**: Use `snake_case` with a `_s` suffix (e.g., `rudp_header_s`, `rudp_context_s`).
- **Unions**: Use `snake_case` with a `_u` suffix (e.g., `tlv_packet_u`).
- **Constants/Macros**: Use `UPPER_SNAKE_CASE` (e.g., `RUDP_WINDOW_SIZE`).

## Formatting

- **Indentation**: The codebase currently uses a mix of **2 and 4 spaces**. For new code, **2 spaces** is preferred to maintain consistency with `rudp.c`.
- **Brace Style**: Use the **K&R style** (opening brace on the same line).
- **Control Statements**: Braces are encouraged but can be omitted for simple single-line `if` statements as seen in `rudp.c`.

## Documentation

- **Headers**: Use Doxygen-style blocks `/** ... */` for structures and function prototypes.
- **Implementation**: Use block comments `/* ... */` or double-stars `/** ... */` for internal logic descriptions.
- **Visual Aids**: ASCII diagrams are welcome to explain complex data structures or flows.

## Memory Management

- **Zero Dynamic Allocation**: Do not use `malloc`, `calloc`, `realloc`, or `free`.
- All memory must be allocated on the stack or as static/global variables.
- Use `memset` for initializing structures when necessary.
