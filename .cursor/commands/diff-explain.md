# Explain Git Diff

Explain what a git diff does in plain English.

**Process:**
1. Read the git diff (from selection, or ask user for commit range/path)
2. For each change:
   - What was changed (file, function, logic)
   - Why it might have been changed (infer from context)
   - What the impact is (what breaks, what improves)
3. Group related changes together

**Output format:**
- **Summary:** One sentence overview
- **Files changed:** List with brief description per file
- **Key changes:** 
  - What was added/removed/modified
  - Why it matters (bug fix, feature, refactor, performance)
- **Impact:** What this affects downstream
- **Questions:** If something looks odd or incomplete, ask

**Sentinel-specific things to watch for:**
- Qt contamination in core layer
- Threading violations
- Viewport version handling (must increment on changes)
- Rendering path changes (hot path implications)
- Architecture violations (core vs GUI separation)

Be concise but thorough. Focus on understanding intent, not just listing changes.
