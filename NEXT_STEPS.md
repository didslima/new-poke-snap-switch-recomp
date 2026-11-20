# New Pokémon Snap Recomp — Linux Port Roadmap

- **Repository:** `https://github.com/didslima/new-poke-snap-switch-recomp-linux`
- **Active branch:** `integration/upstream-v1.3.0`
- **Current release:** `v1.3.0-linux.1-rc1` (pre-release)

---

## Completed Milestones

- Linux ROMFS path configuration and preflight validation before entering `nnMain()`.
- CLI support for `--romfs <folder>` with precedence `--romfs` > `SNAP_ROMFS` > `extracted/romfs-v262144`.
- Built-in graphical launcher using SDL3 (`SDL_Renderer`) and Dear ImGui (`v1.93.0`).
- Automatic discovery of required ELF/archive artifacts and ROMFS with persistent XDG configuration (`$XDG_CONFIG_HOME/new-pokemon-snap-recomp/config`).
- KDE Plasma desktop integration (`.desktop` entry and icon installation under `$XDG_DATA_HOME`, native `xdg-desktop-portal` file/folder pickers).
- Manual Bazzite / KDE Plasma validation.
- Published pre-release `v1.3.0-linux.1-rc1` (preserving upstream `Snap Recomp 1.3.0` and stable release `v1.3.0`).

---

## Prioritized Pipeline

### P1 — Launcher Update Notifications *(Planned — Not Yet Implemented)*

Allow the graphical launcher to detect newer Linux-port releases without interfering with offline use or normal game startup.

#### Requirements
- Query GitHub Releases for `didslima/new-poke-snap-switch-recomp-linux`.
- Identify the latest Linux-port version (distinguishing Linux-port tags/versions from upstream `1.3.0`).
- Distinguish stable releases from pre-releases.
- Allow the user to choose whether to include pre-releases.
- Display a non-intrusive notification in the launcher UI when a newer version is available.
- Provide a direct link to the GitHub release page.
- Never download or install updates automatically.
- Do not interrupt or block normal game startup.
- Cache the last successful check result locally.
- Avoid checking on every launch by using a configurable check interval.
- Handle offline conditions, network/API errors, HTTP rate limits, and malformed JSON responses gracefully.
- Never transmit ROMFS paths, save paths, game data, or personal information.
- Do not require authentication for public release checks.

#### Acceptance Criteria
- No update available: no intrusive notification is shown.
- Stable update available: notification displays the new version and a link to the release page.
- Pre-release available: notification is only shown when the user has enabled pre-release checks.
- Offline or API failure: the launcher continues working normally without blocking **Play**.
- Repeated launches within the configured interval reuse the cached result without issuing new network requests.
- The user can manually trigger an update check from the launcher UI.

#### Testing Plan
- Mock GitHub Releases API responses in unit tests.
- Test version comparison across stable and pre-release Linux-port version strings.
- Test network failures, timeouts, HTTP rate limiting (`403`/`429`), and malformed responses.
- Test cache persistence and configurable check intervals.
- Verify that requests contain no ROMFS paths, save paths, game data, or personal information.

---

## Future Pipeline *(Lower-Priority Ideas — Not Yet Scheduled / Not Committed Scope)*

- **P2:** Improve launcher diagnostics and log access.
- **P3:** Add configurable graphics presets.
- **P4:** Improve installation and KDE integration documentation.
