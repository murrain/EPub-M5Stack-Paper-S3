# Quick Start Guide

Your fork is ready! Here's how to start developing.

## Repository Structure

```
Your Fork: https://github.com/murrain/EPub-M5Stack-Paper-S3
Upstream: https://github.com/juicecultus/EPub-M5Stack-Paper-S3

Branches:
├── master                      (stable releases)
├── develop                     (active development)
├── feature/power-management    (deep/light sleep)
├── feature/jump-to-page        (navigation improvements)
├── test/infrastructure         (CI/CD and unit tests)
└── refactor/smart-pointers     (C++17 modernization)
```

## Quick Commands

### Start working on a feature
```bash
# Power management
git checkout feature/power-management

# Navigation
git checkout feature/jump-to-page

# Testing
git checkout test/infrastructure

# Refactoring
git checkout refactor/smart-pointers
```

### Build and test
```bash
# Build for Paper S3
pio run -e paper_s3

# Flash to device
pio run -e paper_s3 -t upload

# Linux build (for testing)
pio run -e linux_release

# Run tests
pio test -e linux_tests
```

### Commit and push
```bash
# Make changes, then:
git add .
git commit -m "feat(power): implement deep sleep"
git push origin feature/power-management

# Create PR on GitHub
```

## What's Next?

Choose where to start based on your interest:

### Option 1: Power Management (HIGH IMPACT)
**Branch**: `feature/power-management`
**File**: `lib_esp32/EPub_InkPlate/src/inkplate_platform.cpp:70-87`

Implement deep sleep to extend battery life from hours to weeks.

```bash
git checkout feature/power-management
# Edit inkplate_platform.cpp
# Implement esp_deep_sleep_start() with GPIO wake
# Test on hardware
```

### Option 2: Jump to Page (USER VISIBLE)
**Branch**: `feature/jump-to-page`
**Files**:
- `src/controllers/book_controller.cpp:78-149`
- `src/viewers/book_viewer.cpp`

Add ability to jump directly to a page number.

```bash
git checkout feature/jump-to-page
# Add navigation menu item
# Implement page number input
# Use page_locs.get_page_id() for navigation
```

### Option 3: Testing Infrastructure (FOUNDATION)
**Branch**: `test/infrastructure`
**Files**: Create new test files

Set up comprehensive testing before making major changes.

```bash
git checkout test/infrastructure
# Create test fixtures
# Add model tests (PageLocs, EPub, Fonts)
# Verify CI/CD passes
```

### Option 4: Smart Pointers Refactoring (CODE QUALITY)
**Branch**: `refactor/smart-pointers`
**Files**:
- `src/models/epub.cpp` (25 allocations)
- `src/models/books_dir.cpp` (15 allocations)

Modernize memory management to prevent leaks.

```bash
git checkout refactor/smart-pointers
# Start with epub.cpp
# Replace raw pointers with unique_ptr
# Add tests to verify no regressions
```

## Development Workflow

1. **Choose a branch** and check it out
2. **Make changes** following CONTRIBUTING.md guidelines
3. **Test locally** (build + hardware test)
4. **Commit** with conventional commit messages
5. **Push** to your fork
6. **Create PR** from feature branch → develop
7. **CI runs** automatically (GitHub Actions)
8. **Merge** when tests pass and reviewed

## CI/CD Status

Check your Actions tab: https://github.com/murrain/EPub-M5Stack-Paper-S3/actions

The CI will:
- ✓ Build Linux version
- ✓ Run tests
- ✓ Build ESP32 firmware
- ✓ Check code quality

## Resources

- **Roadmap**: See [ROADMAP.md](ROADMAP.md) for the complete plan
- **Contributing**: See [CONTRIBUTING.md](CONTRIBUTING.md) for guidelines
- **Issues**: https://github.com/murrain/EPub-M5Stack-Paper-S3/issues
- **Upstream**: https://github.com/juicecultus/EPub-M5Stack-Paper-S3

## Getting Help

- Open an issue with questions
- Check CONTRIBUTING.md for code style
- Review ROADMAP.md for project direction

---

**Ready to start?** Pick a branch and dive in!

```bash
# Recommended starting point: Power Management
git checkout feature/power-management
```

Good luck! 🚀
