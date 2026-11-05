# Publishing these changes as a genuine GitHub fork

This ZIP is a **prepared source snapshot**, not a Git repository and not itself a GitHub fork. Do not create a new independent repository if you want GitHub to display the "forked from MatixYo/ESP32-Plane-Radar" relationship.

1. Open https://github.com/MatixYo/ESP32-Plane-Radar and choose **Fork** under your own GitHub account.
2. Clone **your fork** (not the original). Your clone contains `.git/` and retains the upstream commit history and fork relationship.
3. Create a branch in that clone for this customized firmware. Copy the files from this prepared ZIP into the clone, replacing the matching source files, while keeping the `.git/` directory.
4. Review tracked files and remove upstream files that are no longer used, if any. **Do not** copy the original upstream wiring examples unchanged into this board-specific firmware's README; this project has a different pinout.
5. Preserve the upstream MIT `LICENSE`, existing Git history and any useful GitHub Actions workflows. Review the upstream CI workflows before relying on them to build this modified PlatformIO project.
6. Run PlatformIO build/tests and inspect the diff (`git status`, `git diff`). Avoid committing `.pio/`, device credentials, binaries from stale builds or personal configuration.
7. Commit and push the branch. After CI builds successfully, merge to your fork's default branch and create a release tag such as `v2.0.0`.

### Why v2.0.0?

The original repository uses semantic release tags (such as `v1.1.5`). This fork has substantial incompatible hardware/configuration and presentation changes. A separate `v2.0.0` tag avoids implying that the modified firmware is identical to upstream or includes every upstream patch. The internal archival label **V17.37 STABLE** remains unchanged for rollback.

### Before publishing

- This snapshot uses a generic public default position; set your personal coordinates in the Wi-Fi/General configuration after flashing.
- The HTTPS client currently uses `setInsecure()` and skips certificate verification. Disclose this in README (already included).
- The semantic-version firmware source has **not** been separately built on the ESP32; V17.37 is the version confirmed by user testing. Build and hardware-test the public release before publishing a ready-to-flash binary.
- GitHub workflow files from the original repository are **not bundled** in this snapshot. Keep and inspect those in the real fork.
