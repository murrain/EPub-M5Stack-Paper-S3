# EPub-M5Stack-Paper-S3 Improvement Roadmap

## Vision
Transform this EPUB reader into a polished, power-efficient, and feature-rich e-reader for the M5Stack Paper S3, with modern C++ code quality and comprehensive testing.

## Focus Areas

### 1. Power Management ⚡
**Goal**: Extend battery life from hours to days/weeks with proper sleep modes

- [ ] Implement deep sleep with GPIO wake
- [ ] Implement light sleep for short timeouts
- [ ] Add power state management
- [ ] Optimize display refresh patterns
- [ ] WiFi power save mode

**Priority**: HIGH | **Status**: Not Started

### 2. Enhanced Navigation 🧭
**Goal**: Provide intuitive navigation with progress indicators and quick jumps

- [ ] Jump to page number feature
- [ ] Progress bar with chapter info
- [ ] Quick jump menu (25%, 50%, 75%, etc.)
- [ ] Location history (last 5 positions)
- [ ] Enhanced touch zones configuration

**Priority**: HIGH | **Status**: Not Started

### 3. Testing Infrastructure 🧪
**Goal**: Achieve 50%+ code coverage with automated testing

- [ ] CI/CD pipeline (GitHub Actions)
- [ ] Code coverage reporting
- [ ] Unit tests for models (PageLocs, EPub, Fonts)
- [ ] Controller tests with mocks
- [ ] Integration tests
- [ ] Performance regression tests

**Priority**: MEDIUM | **Status**: Not Started

### 4. Code Modernization 🔧
**Goal**: Modernize to C++17 standards and improve memory safety

- [ ] Replace raw pointers with smart pointers
- [ ] Adopt std::optional, std::string_view
- [ ] Add structured bindings and modern idioms
- [ ] RAII wrappers for C resources
- [ ] Const correctness and move semantics
- [ ] Static analysis integration

**Priority**: MEDIUM-HIGH | **Status**: Not Started

## Implementation Phases

### Phase 1: Immediate Impact (Weeks 1-3)
**Goal**: Quick wins with visible user impact

1. Deep sleep implementation
2. Jump-to-page feature
3. CI/CD setup with basic tests
4. Repository infrastructure

**Milestone**: v2.1.0

### Phase 2: Foundation (Weeks 4-7)
**Goal**: Establish solid foundation for future work

1. Smart pointer refactoring (epub.cpp, books_dir.cpp)
2. Model test coverage (40%+)
3. Progress bar and chapter display
4. Light sleep implementation

**Milestone**: v2.2.0

### Phase 3: Polish (Weeks 8-10)
**Goal**: Complete the improvements and optimize

1. Controller and integration tests
2. Remaining file refactoring
3. Performance profiling
4. Documentation updates

**Milestone**: v2.3.0

## Success Metrics

| Metric | Current | Target |
|--------|---------|--------|
| Battery Life (reading) | ~8 hours | 2+ weeks |
| Test Coverage | <5% | 50%+ |
| Manual Memory Allocs | 131 | <20 |
| Smart Pointer Usage | ~8 uses | Primary pattern |
| Navigation Features | Basic | Advanced |

## Long-term Vision (Beyond v2.3.0)

- Bookmarks and annotations
- Dictionary integration
- Night mode / inverted display
- PDF support
- Cloud sync capabilities
- OTA firmware updates

## Contributing

See [CONTRIBUTING.md](CONTRIBUTING.md) for development guidelines.

## License

Maintains MIT License compatibility with upstream project.

---

**Last Updated**: 2026-01-14
**Maintainers**: @[your-username]
**Upstream**: https://github.com/juicecultus/EPub-M5Stack-Paper-S3
