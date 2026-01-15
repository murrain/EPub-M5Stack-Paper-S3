# Fork Setup Guide

This guide will help you set up your fork of EPub-M5Stack-Paper-S3 for development.

## Step 1: Create Your Fork on GitHub

1. **Go to GitHub**: Visit https://github.com/juicecultus/EPub-M5Stack-Paper-S3

2. **Click "Fork"**: In the top-right corner, click the "Fork" button

3. **Choose destination**: Select your GitHub account

4. **Wait for fork**: GitHub will create a copy under `https://github.com/YOUR-USERNAME/EPub-M5Stack-Paper-S3`

## Step 2: Update Your Local Repository

You already have a clone of the original repository. Now let's add your fork as a remote:

```bash
# Make sure you're in the project directory
cd /home/patrick/Documents/code/EPub-M5Stack-Paper-S3

# Add your fork as 'origin' (replace YOUR-USERNAME)
git remote rename origin upstream
git remote add origin https://github.com/YOUR-USERNAME/EPub-M5Stack-Paper-S3.git

# Verify remotes
git remote -v
# Should show:
# origin    https://github.com/YOUR-USERNAME/EPub-M5Stack-Paper-S3.git (fetch)
# origin    https://github.com/YOUR-USERNAME/EPub-M5Stack-Paper-S3.git (push)
# upstream  https://github.com/juicecultus/EPub-M5Stack-Paper-S3.git (fetch)
# upstream  https://github.com/juicecultus/EPub-M5Stack-Paper-S3.git (push)
```

## Step 3: Create Development Branch Structure

```bash
# Create and push develop branch (integration branch)
git checkout -b develop
git push -u origin develop

# Create feature branches for each improvement area
git checkout -b feature/power-management
git push -u origin feature/power-management

git checkout develop
git checkout -b feature/jump-to-page
git push -u origin feature/jump-to-page

git checkout develop
git checkout -b test/infrastructure
git push -u origin test/infrastructure

git checkout develop
git checkout -b refactor/smart-pointers
git push -u origin refactor/smart-pointers

# Return to develop
git checkout develop
```

## Step 4: Push Initial Improvements

Let's commit and push the documentation we've created:

```bash
# Make sure you're on develop branch
git checkout develop

# Stage the new documentation files
git add ROADMAP.md CONTRIBUTING.md FORK_SETUP.md .github/

# Commit
git commit -m "docs: add roadmap, contributing guide, and CI/CD workflow

- Add ROADMAP.md with improvement plan and phases
- Add CONTRIBUTING.md with development guidelines
- Add GitHub Actions workflow for CI/CD
- Add FORK_SETUP.md with fork setup instructions

This establishes the foundation for the improvement project focusing on:
1. Power management (deep/light sleep)
2. Enhanced navigation (jump-to-page, progress bar)
3. Testing infrastructure (CI/CD, unit tests)
4. Code modernization (C++17, smart pointers)"

# Push to your fork
git push -u origin develop

# Also push to master if you want
git checkout master
git merge develop
git push -u origin master
```

## Step 5: Set Up GitHub Repository Settings

On GitHub (https://github.com/YOUR-USERNAME/EPub-M5Stack-Paper-S3):

### Enable Actions
1. Go to "Actions" tab
2. Click "I understand my workflows, go ahead and enable them"

### Branch Protection (Optional but Recommended)
1. Go to Settings → Branches
2. Add rule for `master`:
   - Require pull request reviews before merging
   - Require status checks to pass (CI)
   - Require branches to be up to date

3. Add rule for `develop`:
   - Require status checks to pass (CI)

### Set Default Branch
1. Go to Settings → General → Default branch
2. Change to `develop` (for active development)

## Step 6: Verify CI/CD is Working

1. Go to "Actions" tab on your GitHub fork
2. You should see workflows running for your pushed commits
3. Check that both "linux-test" and "esp32-paper-s3-build" jobs pass

## Step 7: Keep Fork Updated

To sync with upstream changes:

```bash
# Fetch upstream changes
git fetch upstream

# Merge into your develop branch
git checkout develop
git merge upstream/master

# Push to your fork
git push origin develop
```

## Branch Workflow Summary

```
YOUR FORK (origin)                    UPSTREAM (upstream)
├── master (stable releases)          ├── master
├── develop (active dev)              └── experimental
├── feature/power-management
├── feature/jump-to-page
├── feature/progress-bar
├── test/infrastructure
└── refactor/smart-pointers
```

### Development Flow

1. **Work on feature branch**: Make changes in `feature/X`
2. **Test locally**: `pio run -e paper_s3`
3. **Commit**: Follow conventional commits
4. **Push**: `git push origin feature/X`
5. **Create PR**: From `feature/X` → `develop` on your fork
6. **CI passes**: GitHub Actions runs tests
7. **Merge**: Merge to `develop` after review
8. **Release**: Merge `develop` → `master` when stable

## Next Steps

After setup, you can start working on improvements:

```bash
# Start with power management
git checkout feature/power-management

# Or start with navigation
git checkout feature/jump-to-page

# Or start with testing
git checkout test/infrastructure
```

See [ROADMAP.md](ROADMAP.md) for the implementation plan and [CONTRIBUTING.md](CONTRIBUTING.md) for development guidelines.

---

**Questions?** Open an issue on your fork or check existing discussions.
