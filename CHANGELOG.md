# Changelog

## 1.6.5

### Added
- Titles launched without selecting a user (homebrew forwarders, emulators, ports) can now be shown in a user's activity.
  - New setting under Settings -> Other: **Show titles without user** (enabled by default, stored as `showUnassigned` in `config.ini`).
  - Sessions that contain no account event at all are accepted for the selected user. Sessions bound to an account keep being separated per user.
  - Only installed applications are accepted, so system applets and hbloader are not shown.
  - First/last played, launch count and playtime for these titles are computed from the play events, since the system only keeps per-account statistics.
- English fallback texts for the new setting (translations still need to be added to the translations repository).

### Notes
- These sessions belong to no account, so they appear for every user (and in every user's exported data).
- Titles that were only ever launched without a user are added to the list at startup. After toggling the setting, restart the app to fully apply it.
- Homebrew opened through hbmenu in title takeover mode is still recorded by the Switch as the hosted title and cannot be separated.

### Credits
- Idea and reference implementation: issue #27 on zdm65477730/NX-Activity-Log.
