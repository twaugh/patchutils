# Contributing to patchutils

## Branching Strategy

We use a dual-branch strategy for development and releases:

### Branches

- **`master`**: Development branch for upcoming 0.5.0 release
  - New features and major changes go here
  - This is where active development happens

- **`0.4.x`**: Stable release branch for 0.4.x series
  - Bug fixes and minor improvements for stable releases
  - Security patches and critical fixes
  - No new features or breaking changes

### Contributing Guidelines

#### For Bug Fixes (Stable Release)
1. Create your feature branch from `0.4.x`
2. Make your changes
3. Submit a PR targeting `0.4.x`
4. Maintainers will cherry-pick to `master` if appropriate

#### For New Features (Development)
1. Create your feature branch from `master`
2. Make your changes
3. Submit a PR targeting `master`

#### Important Notes
- **Do not create PRs from `0.4.x` to `master`**
- The `0.4.x` branch should only receive targeted bug fixes
- New features should go to `master` for the 0.5.0 release

## Development Setup

[Include your existing development setup instructions here]

## Testing

[Include your testing guidelines here]
