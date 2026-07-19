# BigBirdTouch AirPlay Receiver — Third-Party Notices

This executable is a modified build of UxPlay and is distributed under
GPL-3.0-or-later. The corresponding source is published at
https://github.com/zhang3c3c33c/bbt-airplay-receiver and each binary release
identifies its exact source tag and commit.

## Source components

| Component | License | Source |
|---|---|---|
| BigBirdTouch AirPlay Receiver | GPL-3.0-or-later | https://github.com/zhang3c3c33c/bbt-airplay-receiver |
| UxPlay upstream | GPL-3.0-or-later | https://github.com/FDH2/UxPlay |
| PlayFair | GPL-3.0-or-later | `lib/playfair/LICENSE.md` in corresponding source |
| llhttp | MIT | `lib/llhttp/LICENSE-MIT` in corresponding source |

## Windows runtime libraries

| Runtime | License identifier |
|---|---|
| libstdc++ / libgcc / libwinpthread | GPL-3.0-or-later WITH GCC-exception-3.1 |
| OpenSSL 3 | Apache-2.0 |
| GLib | LGPL-2.1-or-later |
| libplist | LGPL-2.1-or-later |
| GNU gettext runtime / libintl | LGPL-2.1-or-later |
| GNU libiconv | LGPL-2.1-or-later |
| PCRE2 | BSD-3-Clause |

License texts supplied by the MSYS2 UCRT64 packages used for the build are
copied to `licenses/airplay/runtime/` by the staging script. System DLLs
provided by Windows are not redistributed.
