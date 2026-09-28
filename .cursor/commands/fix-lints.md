# Fix Linter Errors

Fix linter errors in the current file intelligently.

**Process:**
1. Read linter errors for the current file
2. For each error:
   - Understand what the linter is complaining about
   - Fix it in a way that matches Sentinel's coding style
   - Don't introduce workarounds or suppressions unless absolutely necessary
3. Preserve code intent - don't "fix" things that are intentionally written that way

**Common fixes:**
- Missing includes
- Unused variables (remove if truly unused, or mark with `[[maybe_unused]]` if needed for API)
- Const correctness
- Type mismatches
- Modern C++ replacements (e.g., `nullptr` instead of `NULL`)

**Rules:**
- Match existing code style in the file
- Don't change logic unless the linter error indicates a real bug
- For warnings about performance (e.g., copies), suggest optimizations but don't force them
- If a fix would require architectural changes, flag it and ask

**Output:**
- List each fix made
- Explain why if non-obvious
- Flag any fixes that might need review (logic changes, architectural implications)

If errors are ambiguous or fixing them would break functionality, ask rather than guessing.
