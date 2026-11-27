# foo_discord_rich (Spotify-Style RPC & Fast Cover Art Edition)

An enhanced fork of **Discord Rich Presence Integration** (`foo_discord_rich`, originally created by [TheQwertiest](https://github.com/theqwertiest/foo_discord_rich)) for **foobar2000 x64**. This build delivers a Spotify-like **Listening** status with a live progress bar and replaces external uploader executables with fast, in-memory cover art compression and direct HTTPS uploading.

## Key Enhancements

- **Spotify-Style Listening Rich Presence:**
  - Sends both `startTimestamp` and `endTimestamp` in `LISTENING` activity mode so Discord renders a real-time progress bar (automatically hidden when paused and resynced upon resuming).
  - Injects `"status_display_type": 1` into the Discord IPC payload so the member sidebar displays `Listening to [Artist]` instead of the application name.
- **In-Memory WIC Cover Compression:**
  - Uses Windows Imaging Component (WIC) to downscale high-resolution album artwork (1–8 MB FLAC/Hi-Res covers) to `300x300` JPEG (~20–30 KB) directly in RAM, avoiding temporary disk writes and external `.exe` subprocess overhead.
- **Direct HTTPS Upload with Automatic CDN Fallback:**
  - Uploads compressed cover art directly via `libcurl`/`cpr` to a Discord-proxy-compatible CDN (`iili.io` via `freeimage.host`), automatically falling back to `catbox.moe` if the primary host is unreachable.
- **Two-Tier Smart Artwork Caching:**
  - Persists only verified permanent `https://` artwork URLs to disk (`art_urls.v2.0.1.json`), while transient upload errors are placed in a 45-second in-memory cooldown instead of permanently locking the album in cache.
- **Visual Studio 2026 & C++20/23 Build Support:**
  - Updated project toolsets (`v145`), Windows SDK (`10.0.26100.0`), CMake 4.x policies, and C++20/23 concept constraints for clean compilation on modern toolchains.

---

## Installation

1. Download the latest **`foo_discord_rich.fb2k-component`** from the **[Releases](../../releases)** page.
2. Open **foobar2000** and navigate to **File → Preferences → Components**.
3. Click **Install...**, select the downloaded `.fb2k-component` file, and click **Apply** to restart foobar2000.

---

## Recommended Configuration (Spotify Layout)

Go to **File → Preferences → Tools → Discord Rich Presence Integration** and configure both tabs as follows:

### 1. `Main` Tab
- **Enable Rich Presence:** Checked
- **Text fields:**
  - **Top:** `[%title%]`
  - **Middle:** `$if2($meta_sep(artist,', '),%artist%)`
  - **Bottom:** `$if($stricmp(%album%,%title%),,[%album%])`
- **Foobar2000 image:** `Disabled` *(prevents the default placeholder icon from appearing while new cover art is uploading)*
- **Playback status image:** `Disabled`
- **Track duration:** `Elapsed time` *(required for Discord to render the Spotify-style progress bar)*

### 2. `Advanced` Tab
- **Discord application token:** Enter your Application ID from the [Discord Developer Portal](https://discord.com/developers/applications) *(your application name on the portal appears after `Listening to ...` in your profile card)*.
- **Art uploader:**
  - Check **Upload and display art (disables MusicBrainz fetcher)**.
  - **Upload command:** Enter `catbox` *(or any path containing `catbox` to activate the built-in WIC + HTTPS uploader)*.
  - **Art pin query:** `%album artist%|%album%` *(ensures all tracks belonging to the same album share a single cached cover URL)*.

---

## Building from Source

### Prerequisites
- Windows 10/11 (x64)
- **Visual Studio 2026** (or VS 2022) with the **Desktop development with C++** workload and Windows 11 SDK (`10.0.26100.0`)
- **Python 3** (for component packaging)

### Build Steps
1. Clone the repository:
   ```powershell
   git clone [https://github.com/vuismei/foo_discord_rich.git](https://github.com/vuismei/foo_discord_rich.git)
   cd foo_discord_rich
   ```
2. Open **`workspaces\foo_discord_rich.sln`** in Visual Studio.
3. Select the **`Release`** configuration and **`x64`** platform on the toolbar, then press **Ctrl + Shift + B** to build.
4. Package the `.fb2k-component` archive:
   ```powershell
   py scripts\pack_component.py --configuration Release --platform x64
   ```
   The compiled component will be generated at `_result\x64_Release\foo_discord_rich.fb2k-component`.

---

## Credits & License

- Original component architecture by **TheQwertiest** ([theqwertiest/foo_discord_rich](https://github.com/theqwertiest/foo_discord_rich)).
- Distributed under the terms of the [LICENSE](LICENSE) file.