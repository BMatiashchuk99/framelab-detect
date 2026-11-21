# FrameLab Detect

A tiny Windows program (C, Win32, about 150 KB, no installer) behind the **Detect my specs** button on
[framelab-fps.com](https://framelab-fps.com): the FPS calculator, *What can I run* and the PSU calculator.
It reads the processor, graphics card, memory and screen of the PC it runs on and hands them to the FrameLab page
that asked for them, so the visitor does not have to look up their exact models.

Nothing to click: the visitor presses the button, runs the download, and the page fills itself in.

## How it works

1. The page creates a random one-time code (16 characters `a-z`, `2-7`) and starts the download as
   `FrameLab-Detect-<locale>-<tool>-<code>.exe`. Then it asks `https://framelab-fps.com/api/detect/<code>` once a
   second whether the values have arrived.
2. The program reads the code and the language from its own file name and tells that address that it has started
   (`fl-pc&v=1&status=scanning`).
3. It reads:
   - the processor name (registry value `HKLM\HARDWARE\DESCRIPTION\System\CentralProcessor\0\ProcessorNameString`),
   - the graphics adapters with their dedicated memory (DXGI; the Microsoft Basic Render Driver is skipped),
   - the installed memory (`GetPhysicallyInstalledSystemMemory`),
   - the primary screen's display mode (`EnumDisplaySettings`),

   and sends them to the same address with `&status=done`.
4. The site keeps them in memory only, for at most 15 minutes, and deletes them as soon as the page has read them.
   The page matches the names to its catalog and fills in the tool.
5. The program shows the same values with a progress bar and closes itself after 8 seconds.

If the file has no code (it was renamed) or the site cannot be reached, the program opens
`https://framelab-fps.com/<locale>/<tool>#fl-pc&v=1&cpu=…` in the default browser instead. Browsers never send the
part after `#` to the server; the site moves it out of the address before any other script runs.

### What is sent

Exactly this, nothing else (one `gpu`/`vram` pair per graphics adapter, `vram` in MB, names URL-encoded):

```
fl-pc&v=1&cpu=AMD%20Ryzen%205%208400F%206-Core%20Processor&gpu=NVIDIA%20GeForce%20RTX%204060&vram=8188&ram=32&res=2560x1440&status=done
```

### What it does not do

- It reads no serial numbers, MAC addresses, user or computer names, product keys or installed software.
- It writes nothing to disk or the registry, installs nothing, keeps no ID and does not start with Windows.
- It does not update itself and downloads nothing.

## Privacy

This program will not transfer any information to other networked systems unless specifically requested by the
user or the person operating it. It sends the values above only to the FrameLab page the user started it from,
after the user pressed **Detect my specs** there and ran the download. The program window shows what is sent and
links to the [FrameLab privacy policy](https://framelab-fps.com/en/privacy) (section "Automatic hardware detection").

## Command-line options

| Option | What it does |
| --- | --- |
| `--print` | Detects and writes the fallback address to standard output instead of showing the window. Sends nothing. |
| `--site URL` | Talks to another FrameLab address, for example `--site http://localhost:3000` for a local development server. |

## Build

```sh
sudo apt install gcc-mingw-w64-x86-64   # macOS: brew install mingw-w64
./build.sh                              # -> dist/FrameLab-Detect.exe
```

The build has no timestamp, so the same source and compiler give the same file. Releases are built only by
[GitHub Actions](.github/workflows/build.yml): push a `v*` tag, or run the *build* workflow by hand with the
`release` input (for example `v1.2.0`), which creates the tag. The workflow attaches the `.exe` and its SHA-256 to
the GitHub release.

## Code signing policy

Status: application to SignPath Foundation pending. Until it is accepted, releases are unsigned.

Free code signing provided by [SignPath.io](https://about.signpath.io), certificate by
[SignPath Foundation](https://signpath.org).

- Releases are built from this repository by GitHub Actions and signed only from that build. Nothing built on a
  developer PC is signed.
- Every signing request is approved by hand.
- Team roles:
  - Committers and reviewers: [BMatiashchuk99](https://github.com/BMatiashchuk99)
  - Approvers: [BMatiashchuk99](https://github.com/BMatiashchuk99)
- All team members use multi-factor authentication for GitHub and SignPath.
- Changes from people outside the team (pull requests) are reviewed by a team member before they are merged.

## License

[MIT](LICENSE)
