# Changelog

## 1.6.6

### Added
- Profiles that were deleted from the console are listed on the user selection screen as "Deleted profile 1, 2, ...", with the games they played and their playtime. The Switch keeps the play events of deleted profiles, so nothing is lost. New setting under Settings -> Other: **Show deleted profiles** (enabled by default, `showDeletedProfiles` in `config.ini`; restart the app to apply).
- Optional report to investigate playtime: create an empty file `/config/NX-Activity-Log/debug.flag` and open the app to get `/config/NX-Activity-Log/debug.txt`.

### Changed
- Titles launched without a user no longer disappear after being uninstalled. Any application ID is accepted; system applets and hbloader are still excluded because their IDs are below the range used by applications.
- The name and icon of a title are saved to `/config/NX-Activity-Log/cache` while it is installed, so uninstalled titles (including regular games) keep showing them. Titles with no saved data show their ID and a placeholder icon instead of a blank entry. Titles uninstalled before this version have nothing saved and will show their ID.
- Sessions without an account are only shown for titles that are never played with an account (homebrew, forwarders, emulators, ports), and only on profiles that exist. Regular games started and closed without choosing a profile no longer add playtime to your profile.

### Fixed
- Wrong "last played" dates (impossible years such as -528 or 7246) that changed every time the profile was opened. Statistics that were left uninitialised are now zeroed, and for titles launched without an account and for deleted profiles the app no longer relies on the system's per-account statistics: first/last played, launches and playtime are worked out from the play events. Timestamps before 2000 or in the future are ignored.

## 1.6.5

### Added
- Titles launched without selecting a user (homebrew forwarders, emulators, ports) can be shown in a user's activity.
  - New setting under Settings -> Other: **Show titles without user** (enabled by default, `showUnassigned` in `config.ini`).
  - Sessions that contain no account event at all are accepted for the selected user. Sessions bound to an account keep being separated per user.
  - First/last played, launch count and playtime for these titles are computed from the play events, since the system only keeps per-account statistics.

### Notes
- These sessions belong to no account, so they appear for every user (and in every user's exported data).
- Homebrew opened through hbmenu in title takeover mode is still recorded by the Switch as the hosted title and cannot be separated.

### Credits
- Idea and reference implementation: issue #27 on zdm65477730/NX-Activity-Log.
