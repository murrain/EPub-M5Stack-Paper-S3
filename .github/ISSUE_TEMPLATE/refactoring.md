---
name: Refactoring Task
about: Propose code refactoring or modernization
title: '[REFACTOR] '
labels: refactoring, technical-debt
assignees: ''
---

## Current State
Describe the code that needs refactoring.

## Problem
What issues does the current code have?
- [ ] Memory safety (raw pointers, leaks)
- [ ] Code duplication
- [ ] Poor performance
- [ ] Unclear logic
- [ ] Lacks modern C++ features
- [ ] Other: _____

## Proposed Refactoring
Describe how you'd improve the code.

### Before
```cpp
// Current code example
```

### After
```cpp
// Proposed code example
```

## Benefits
- Improved memory safety
- Better performance
- More maintainable
- Follows modern C++ practices
- Other: _____

## Files Affected
List the files that will be modified:
- `src/models/example.cpp`
- `include/models/example.hpp`

## Risks
What could break? How will you mitigate risks?

## Testing Plan
How will you verify the refactoring doesn't break anything?
