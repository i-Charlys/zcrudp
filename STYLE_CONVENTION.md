# Style Convention

This document outlines the coding standards and conventions used in the `zcrudp` project.

## C Standards

- The project follows the **C11** standards.
- Use standard integer types from `<stdint.h>` (e.g., `uint32_t`, `uint16_t`).

## Naming Conventions

- **Functions**: Use `snake_case` (e.g., `rudp_init`, `rudp_send`).
- **Variables**: Use `snake_case` (e.g., `seq_num`, `ack_num`).
- **Structs**: Use `snake_case` with a `_s` suffix (e.g., `rudp_header_s`, `rudp_context_s`).
- **Unions**: Use `snake_case` with a `_u` suffix (e.g., `tfv_packet_u`).
- **Constants/Macros**: Use `UPPER_SNAKE_CASE` (e.g., `RUDP_WINDOW_SIZE`).

## Structure initialization

- Use C11 designated initializers for structures and unions: explicitly name
  each supplied field, including nested members. Avoid positional initializers.
- Example: `rudp_header_s header = {.seq_num = 0, .ack = 1};`.
- For a complete value written into an existing object, pointer destination or
  array element, use a designated compound literal:
  `col->iovecs[col->count] = (rudp_iovec_s){.base = base, .len = len};`.
- Omitted fields are zero-initialized by C. Keep individual assignments for
  partial state updates and keep direct structure copies when copying a value.
- When filling a reused or variable-length payload buffer, write only the valid
  bytes and update its metadata. Do not introduce a full-buffer clear just to
  replace those partial writes with a compound literal.

## Formatting

- **Indentation**: Use **2 spaces**, with the repository `.clang-format` as the source of truth.
- **Brace Style**: Use the **K&R style** (opening brace on the same line).
- **Control Statements**: Braces are encouraged but can be omitted for simple single-line `if` statements when unambiguous.

## Documentation

- **Headers**: Use Doxygen-style blocks `/** ... */` for structures and function prototypes.
- **Implementation**: Use block comments `/* ... */` or double-stars `/** ... */` for internal logic descriptions.
- **Visual Aids**: ASCII diagrams are welcome to explain complex data structures or flows.

## Memory Management

- **Zero Dynamic Allocation**: Do not use `malloc`, `calloc`, `realloc`, or `free`.
- All memory must be allocated on the stack or as static/global variables.
- Use `memset` for initializing structures when necessary.
