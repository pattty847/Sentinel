# Generate Implementation Stubs

Generate a `.cpp` implementation file from a `.hpp` header file.

**Process:**
1. Read the header file (or selected class/struct)
2. Extract all:
   - Function declarations (public, protected, private)
   - Constructor/destructor signatures
   - Static methods
   - Inline functions (skip these, they're already implemented)
3. Generate corresponding `.cpp` file with:
   - Appropriate includes (header first, then system/Qt, then project)
   - Function stubs with correct signatures
   - Empty bodies with `// TODO: implement` comments
   - Constructor initialization lists (if applicable)

**Rules:**
- Match Sentinel's coding style (modern C++20, RAII, smart pointers)
- Use explicit types, not `auto` for public APIs
- For Qt classes: include proper Q_OBJECT macros if needed
- For core classes: no Qt dependencies (except QString/QDateTime if explicitly needed)
- Preserve const correctness, noexcept, override keywords
- Handle templates appropriately (don't generate .cpp for template-only code)

**Output:**
- Show the generated `.cpp` content
- Suggest where to place it (same directory as header)
- Note any dependencies that need to be added to CMakeLists.txt

If the header is incomplete or unclear, ask for clarification.
