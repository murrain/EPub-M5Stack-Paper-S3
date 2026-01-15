# Contributing to EPub-M5Stack-Paper-S3

Thank you for your interest in improving this EPUB reader for the M5Stack Paper S3!

## Development Setup

### Prerequisites

- **PlatformIO**: For ESP32 firmware builds
- **ESP-IDF v4.3.2**: Automatically managed by PlatformIO
- **Git**: With submodules support
- **Hardware**: M5Stack Paper S3 device (for testing)

### Clone and Setup

```bash
# Fork the repository on GitHub first, then:
git clone --recurse-submodules https://github.com/YOUR-USERNAME/EPub-M5Stack-Paper-S3.git
cd EPub-M5Stack-Paper-S3

# Ensure submodules are initialized
git submodule update --init --recursive

# Add upstream remote
git remote add upstream https://github.com/juicecultus/EPub-M5Stack-Paper-S3.git

# Build for Paper S3
pio run -e paper_s3

# Flash to device
pio run -e paper_s3 -t upload
```

### Branch Strategy

We use a feature-branch workflow:

```
master (stable)
  ├── develop (integration branch)
  │   ├── feature/power-management
  │   ├── feature/jump-to-page
  │   ├── feature/progress-bar
  │   ├── refactor/smart-pointers-epub
  │   └── test/model-coverage
```

- `master`: Stable releases only
- `develop`: Integration branch for testing
- `feature/*`: New features
- `refactor/*`: Code improvements
- `test/*`: Testing infrastructure
- `bugfix/*`: Bug fixes

## Development Guidelines

### Code Style

We follow modern C++17 conventions:

```cpp
// ✅ GOOD: Smart pointers
auto book = std::make_unique<EPub>();

// ❌ BAD: Raw pointers with manual management
EPub* book = new EPub();
delete book;

// ✅ GOOD: std::optional for nullable returns
std::optional<PageId> get_page_id(int page_num);

// ❌ BAD: nullptr returns with no type safety
PageId* get_page_id(int page_num); // returns nullptr on error

// ✅ GOOD: const correctness
const std::string& get_title() const;

// ✅ GOOD: Move semantics for large objects
std::string process_content(std::string&& content);
```

### Memory Management

- **Prefer RAII**: Resources acquired in constructors, released in destructors
- **Smart Pointers**: Use `std::unique_ptr` and `std::shared_ptr`
- **Avoid manual allocation**: No naked `new`/`delete` or `malloc`/`free`
- **ESP32 Considerations**: Mind PSRAM vs DRAM allocation patterns

### Testing

All new features and refactors should include tests:

```cpp
// src/models/page_locs_tests.cpp
#if TESTING && EPUB_LINUX_BUILD

#include "gtest/gtest.h"
#include "models/page_locs.hpp"

TEST(PageLocsTest, GetNextPageId) {
    PageLocs page_locs;
    // ... test implementation
    EXPECT_TRUE(next_page != nullptr);
}

#endif
```

Run tests:
```bash
pio test -e linux_tests
```

### Commit Messages

Follow conventional commits:

```
feat(power): implement deep sleep with GPIO wake
fix(nav): correct page number calculation off-by-one
refactor(epub): replace raw pointers with unique_ptr
test(fonts): add TTF loading test cases
docs(readme): update build instructions for Paper S3
```

Types: `feat`, `fix`, `refactor`, `test`, `docs`, `style`, `perf`, `ci`

## Pull Request Process

1. **Create a feature branch** from `develop`:
   ```bash
   git checkout develop
   git checkout -b feature/your-feature-name
   ```

2. **Make your changes** with tests and documentation

3. **Test thoroughly**:
   ```bash
   # Build for Paper S3
   pio run -e paper_s3

   # Run tests (if on Linux)
   pio test -e linux_tests

   # Test on actual hardware
   pio run -e paper_s3 -t upload
   ```

4. **Commit with clear messages**:
   ```bash
   git add .
   git commit -m "feat(nav): add jump to page number feature"
   ```

5. **Push and create PR**:
   ```bash
   git push origin feature/your-feature-name
   # Then create PR on GitHub targeting 'develop' branch
   ```

6. **PR Checklist**:
   - [ ] Code builds without warnings
   - [ ] Tests pass (if applicable)
   - [ ] Tested on actual hardware
   - [ ] Documentation updated
   - [ ] Commit messages follow convention
   - [ ] No unrelated changes included

## Issue Reporting

When reporting bugs, include:

- **Hardware**: M5Stack Paper S3 model/version
- **Firmware Version**: Git commit hash
- **Steps to Reproduce**: Clear, numbered steps
- **Expected Behavior**: What should happen
- **Actual Behavior**: What actually happens
- **Logs**: Serial output if relevant

## Feature Requests

Feature requests should include:

- **Use Case**: Why is this needed?
- **Proposed Solution**: How should it work?
- **Alternatives**: Other approaches considered?
- **Complexity**: Rough estimate of effort

## Code Review Guidelines

Reviewers will check for:

- ✅ Code correctness and edge cases
- ✅ Memory safety (no leaks, proper RAII)
- ✅ Performance impact on ESP32
- ✅ Test coverage for new code
- ✅ Documentation completeness
- ✅ Consistent code style

## Architecture Decisions

For significant changes, please:

1. Open an issue for discussion first
2. Consider impact on:
   - Memory usage (ESP32 has limited RAM)
   - Performance (e-ink refresh is slow)
   - Battery life (power consumption)
   - User experience (responsiveness)

## Resources

- **PlatformIO Docs**: https://docs.platformio.org
- **ESP-IDF Docs**: https://docs.espressif.com/projects/esp-idf/
- **epdiy Driver**: https://github.com/vroland/epdiy
- **M5Stack Paper S3**: https://docs.m5stack.com/en/core/CoreS3

## License

By contributing, you agree that your contributions will be licensed under the MIT License.

---

**Questions?** Open an issue or discussion on GitHub.
