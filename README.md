# mod_sfv

**SFV checking for ProFTPD.** `mod_sfv` verifies uploaded files against `.sfv` checksum files as they arrive, marks missing or corrupt files, and keeps a live `[INCOMPLETE]` / `[COMPLETE]` status folder in each release directory — all inside ProFTPD, with no external zipscript or `mod_exec` hooks.

```
ftp> put 05-artist-track_five.mp3
226-Transfer complete
226 SFV: CRC OK (3F1A9C42)
```

---

## Features

- **CRC32 verification on upload.** Every file uploaded via `STOR`/`STOU` is checked against any `.sfv` in the same directory (filename match is case-insensitive).
- **Order-independent.** Upload the `.sfv` first or last: when a `.sfv` arrives, files already present are verified on the spot.
- **Missing markers.** Zero-byte `<file>-missing` markers show which files are still needed. They disappear when a good copy arrives.
- **Bad file handling.** Files with a wrong CRC are renamed to `*.bad`, deleted, or kept, as configured, and get a `-missing` marker. A stale `.bad` copy is removed once a good copy passes.
- **Status folders.** A folder such as `[INCOMPLETE 15 of 20F]` is kept up to date and becomes `[COMPLETE 20F]` once every listed file is present and verified. Names are configurable.
- **Delete-aware.** Deleting a listed file restores its marker and updates the status folder; deleting the `.sfv` removes the status folder.
- **Client feedback.** Results are appended to the `226` transfer reply, so they show up in the FTP client's log.
- **Logging.** Optional dedicated log file plus `Trace` support.
- **Per-directory configuration.** Enable or tune the module per `<Directory>` or `<Anonymous>` section.
- **Safe parsing.** `.sfv` entries containing `/`, `.` or `..` are ignored, so a crafted `.sfv` can't touch files outside its own directory.

## Requirements

- ProFTPD **1.3.4 or later** (developed and tested on 1.3.8b)
- ProFTPD development headers and `prxs` for a DSO build
- A C compiler and `libtool`

On Debian / Ubuntu:

```sh
apt install proftpd-dev build-essential libtool
```

## Building

### As a loadable module (DSO) — recommended

```sh
git clone https://github.com/bccrew/mod_sfv.git
cd mod_sfv
prxs -c -i -d mod_sfv.c
```

This compiles the module and installs `mod_sfv.so` into ProFTPD's module directory (`/usr/lib/proftpd` on Debian/Ubuntu).

Then load it, e.g. in `/etc/proftpd/modules.conf` on Debian/Ubuntu:

```apache
<IfModule mod_dso.c>
  LoadModule mod_sfv.c
</IfModule>
```

### Compiled into ProFTPD (static)

```sh
cp mod_sfv.c /path/to/proftpd-source/contrib/
cd /path/to/proftpd-source
./configure --with-modules=mod_sfv
make
make install
```

To combine with other modules, separate them with colons: `--with-modules=mod_tls:mod_sfv`.

### Verify

```sh
proftpd -vv | grep sfv
```

should list `mod_sfv/0.2`.

## Configuration

Minimal setup:

```apache
<IfModule mod_sfv.c>
  SFVEngine          on
  SFVLog             /var/log/proftpd/sfv.log
  SFVBadFiles        rename
  SFVMissingMarkers  on
  SFVStatusDirs      on
</IfModule>
```

Check the configuration and restart:

```sh
proftpd -t && systemctl restart proftpd
```

> **Debian/Ubuntu note:** files in `/etc/proftpd/conf.d/` are only read if `proftpd.conf` contains `Include /etc/proftpd/conf.d/`. Check with `grep -i include /etc/proftpd/proftpd.conf`.

### Directives

| Directive | Values | Default | Context | Description |
|---|---|---|---|---|
| `SFVEngine` | `on` \| `off` | `off` | server, `<Global>`, `<VirtualHost>`, `<Anonymous>`, `<Directory>` | Enables SFV checking. |
| `SFVLog` | path \| `none` | `none` | server, `<Global>`, `<VirtualHost>` | Log file for results. Must be an absolute path; opened before chroot. |
| `SFVBadFiles` | `keep` \| `rename` \| `delete` | `rename` | all | What to do with a file whose CRC doesn't match. `rename` appends `.bad`. |
| `SFVMissingMarkers` | `on` \| `off` | `on` | all | Create `<file>-missing` markers for listed files that are absent or bad. |
| `SFVStatusDirs` | `on` \| `off` | `on` | all | Maintain an incomplete/complete status folder in the release directory. |
| `SFVIncompleteDir` | format | `"[INCOMPLETE %o of %tF]"` | all | Name of the status folder while the release is incomplete. |
| `SFVCompleteDir` | format | `"[COMPLETE %tF]"` | all | Name of the status folder once the release is complete. |

Format variables for the status folder names:

| Variable | Meaning |
|---|---|
| `%o` | number of files present and verified |
| `%t` | number of files listed in the `.sfv` |
| `%m` | number of files missing or bad |
| `%%` | a literal `%` |

A status format must **start with literal text** (for example `[COMPLETE `) and must not contain `/`. The module uses that leading text to recognise and replace its own old status folders. Only empty folders are ever removed, so an unrelated folder that happens to share the prefix is left alone.

### Per-directory example

```apache
SFVEngine on

<Directory /srv/ftp/incoming/tmp>
  SFVEngine off
</Directory>

<Directory /srv/ftp/archive>
  SFVBadFiles      delete
  SFVStatusDirs    off
</Directory>
```

Settings in `<VirtualHost>` sections are not inherited from the main server configuration; repeat the `SFV*` directives inside each `<VirtualHost>` that needs them.

## How it works

The module hooks the `POST_CMD` phase of `STOR`, `STOU` and `DELE`, after the transfer or deletion has succeeded.

**When a regular file is uploaded**, the module looks for a `.sfv` in the same directory that lists it, computes the file's CRC32 and compares:

- **match** → the `-missing` marker and any old `.bad` copy are removed;
- **mismatch** → the file is handled according to `SFVBadFiles` and gets a `-missing` marker;
- **not listed / no `.sfv`** → nothing happens.

**When a `.sfv` is uploaded**, every listed file already present is verified, and markers are created for the rest. The client gets a summary:

```
226 SFV: 18 listed, 17 OK, 1 bad, 0 missing
```

**After every upload or delete**, the release is recounted and the status folder replaced. A file counts as done when it exists and has no `-missing` marker.

Example release directory mid-upload:

```
[INCOMPLETE 17 of 18F]/
00-artist-album-2024.nfo
00-artist-album-2024.sfv
01-artist-track_one.mp3
...
05-artist-track_five.mp3-missing
05-artist-track_five.mp3.bad
06-artist-track_six.mp3
...
```

## Logging and debugging

With `SFVLog` set, each check is logged:

```
mod_sfv/0.2: /incoming/Artist-Album-2024/05-artist-track_five.mp3: BAD CRC (expected 3F1A9C42, got 9B07E115) per 00-artist-album-2024.sfv, user alice, renamed to .bad
mod_sfv/0.2: /incoming/Artist-Album-2024: release complete (18 files)
```

For more detail, enable tracing:

```apache
TraceLog /var/log/proftpd/trace.log
Trace    sfv:10
```

Trace messages explain skipped files, for example when a directory has no `.sfv` or a file isn't listed in it.

If results don't appear in your FTP client, check that it displays multi-line server replies (FileZilla's message log, or `lftp` with `debug 3`). Some clients hide continuation lines of the `226` reply.

## Limitations

- **CRC is computed after the upload completes**, before the `226` reply is sent. Very large files can take a moment; clients with short control-connection timeouts may complain.
- **Renames (`RNFR`/`RNTO`) and `APPE`** don't trigger a check. The status folder corrects itself on the next upload or delete in that directory.
- **When a `.sfv` is processed**, listed files are found by their exact (case-sensitive) name on disk. Single uploads are matched case-insensitively.
- **With `SFVBadFiles keep` and `SFVMissingMarkers off`**, a kept bad file can't be told apart from a good one, so the status folder may report the release as complete.
- **Only CRC32 `.sfv` files** are supported (no MD5/SHA checksum files).

## Compatibility

| ProFTPD | Platform | Status |
|---|---|---|
| 1.3.8b | Ubuntu (proftpd-dfsg 1.3.8.b+dfsg-1ubuntu0.1), DSO via `prxs` | ✅ tested |
| ≥ 1.3.4 | — | should work |
| < 1.3.4 | — | ❌ not supported (refuses to compile) |

Reports for other versions and platforms are welcome.

## License

Released under the GNU General Public License, version 2 or later, the same license as ProFTPD itself. See [`LICENSE`](LICENSE).

## Contributing

Bug reports and pull requests are welcome. Please include your ProFTPD version (`proftpd -vv`), the relevant configuration, and `SFVLog` or `Trace sfv:10` output where possible.
