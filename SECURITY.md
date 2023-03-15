# Security policy for Flatpak

 * [Supported Versions](#Supported-Versions)
 * [Reporting a Vulnerability](#Reporting-a-Vulnerability)
 * [Security Announcements](#Security-Announcements)
 * [Acknowledgements](#Acknowledgements)

## Supported Versions

In stable branches and released packages, this table is likely to be outdated;
please check
[the latest version](https://github.com/flatpak/flatpak/blob/main/SECURITY.md).

| Version  | Supported          | Status
| -------- | ------------------ | -------------------------------------------------------------- |
| 1.15.x   | :hammer:           | Development branch, releases may include non-security changes  |
| 1.14.x   | :white_check_mark: | Stable branch, recommended for use in distributions            |
| 1.13.x   | :x:                | Old development branch, no longer supported                    |
| 1.12.x   | :warning:          | Old stable branch, security fixes applied if feasible          |
| 1.11.x   | :x:                | Old development branch, no longer supported                    |
| 1.10.x   | :warning:          | Old stable branch, security fixes applied if feasible          |
| <= 1.9.x | :x:                | Older branches, no longer supported                            |

The latest stable branch (currently 1.14.x) is the highest priority for
security fixes.
If a security vulnerability is reported under embargo, having new releases
for older stable branches will not always be treated as a blocker for
lifting the embargo.

## Reporting a Vulnerability

If you think you've identified a security issue in Flatpak, please DO NOT
report the issue publicly via the GitHub issue tracker, mailing list,
Matrix, IRC or any other public medium. Instead, send an email with as
many details as possible to
[flatpak-security@lists.freedesktop.org](mailto:flatpak-security@lists.freedesktop.org).
This is a private mailing list for the Flatpak maintainers.

Please do **not** create a public issue.

## Security Announcements

The [flatpak@lists.freedesktop.org](mailto:flatpak@lists.freedesktop.org) email list is used for messages about
Flatpak security announcements, as well as general announcements and
discussions.
You can join the list [here](https://lists.freedesktop.org/mailman/listinfo/flatpak).

## Acknowledgements

This text was partially based on the [github.com/containers security policy](https://github.com/containers/common/blob/main/SECURITY.md).
