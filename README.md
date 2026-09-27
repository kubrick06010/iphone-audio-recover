# iPhone Audio Recover

Recover audio files that are still stored on an iPhone when the original files are missing from a Mac library.

The tool reads the iPhone's local `MediaLibrary.sqlitedb` through AFC, selects tracks by exact artist and album names (case insensitive), then finds the corresponding files under `/iTunes_Control/Music`. It matches both the hashed filename and the recorded file size before copying anything. That second check matters because iOS can contain the same hashed filename in more than one folder.

It only reads the phone. It does not start Finder sync, change the phone library, or delete files.

## Requirements

- macOS
- A paired and trusted iPhone
- [libimobiledevice](https://libimobiledevice.org/)
- SQLite development files

Install the dependencies with Homebrew:

```sh
brew install libimobiledevice sqlite pkg-config
```

## Build

```sh
git clone https://github.com/kubrick06010/iphone-audio-recover.git
cd iphone-audio-recover
make
```

## Find the device identifier

For an iPhone connected over Wi-Fi:

```sh
idevice_id -n
```

For USB, use:

```sh
idevice_id -l
```

The iPhone must already be trusted by the Mac. If Finder shows a sync error, do not start a sync for recovery; use the AFC connection instead.

## Recover an album

First run a dry run. The command downloads a temporary copy of the media database, lists the matching tracks, and shows which phone files would be used:

```sh
./recover_iphone_audio \
  --udid "YOUR-DEVICE-ID" \
  --network \
  --artist "La Vida Bohème" \
  --album "Será" \
  --track-count 15 \
  --dry-run
```

Then copy the files to an empty output directory:

```sh
mkdir -p "./recovered/Será"
./recover_iphone_audio \
  --udid "YOUR-DEVICE-ID" \
  --network \
  --artist "La Vida Bohème" \
  --album "Será" \
  --track-count 15 \
  --output "./recovered/Será"
```

The recovered files keep the audio metadata already embedded in the phone copy and are named with their track number and title. Existing output files are skipped.

Omit `--network` for a USB connection. `--track-count` is useful when the phone contains multiple releases with the same artist and album title. If the database does not record a track count, the option does not filter that result.

## What it can and cannot recover

This is for local audio files that are present in the iPhone's media library. It can recover formats that iOS stores in the local music area, including MP3 files.

It does not bypass DRM, retrieve cloud-only Apple Music downloads, repair a damaged media file, or recreate artwork and playlists. It also does not promise recovery when the iPhone has already removed the local file.

## Safety and privacy

The program copies the media database to a temporary directory under `/tmp`, uses it during the run, and removes it before exiting. No database, device identifier, audio file, or personal library path belongs in this repository.

The project is intentionally small so it can be audited before use. Review the source and run `--dry-run` before copying files.

## License

MIT.
