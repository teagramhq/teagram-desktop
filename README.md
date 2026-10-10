# Teagram Desktop

Teagram Desktop is the macOS client for Teagram. It is an independent project and is not affiliated with Telegram. The client connects to a Teagram server selected during enrollment and pins the server identity to the account. Sign-in uses a username and password only; phone and QR sign-in are not supported.

See [Server enrollment](docs/server_enrollment.md) for server discovery and identity pinning.

## Supported platform

Teagram Desktop is supported on macOS. Linux GUI builds are not a release or merge gate. Windows, Linux, Snap, and Flatpak downloads are not provided by this project.

## Build on macOS

Install Xcode and Homebrew, then install the build dependencies:

```sh
brew install automake libtool meson nasm ninja pkg-config
sudo xcode-select -s /Applications/Xcode.app/Contents/Developer
```

Clone this repository with its submodules and build the macOS client:

```sh
git clone --recursive https://github.com/teagramhq/teagram-desktop.git
cd teagram-desktop
./Telegram/build/prepare/mac.sh skip-release silent
cd Telegram
./configure.sh \
  -D CMAKE_OSX_ARCHITECTURES=arm64 \
  -D CMAKE_CONFIGURATION_TYPES=Debug \
  -D TDESKTOP_API_TEST=ON \
  -D DESKTOP_APP_DISABLE_AUTOUPDATE=ON
cmake --build ../out --config Debug --parallel
```

From the repository root, the app is produced at `out/Debug/Teagram.app`.

## Updates and CI

An updater is planned around signed GitHub Releases, with `dev` and `main` update channels. Apple notarization is not planned.

The macOS build workflow runs on demand. Linux GUI CI is not a gate. Pull request #97 removed tag-triggered release publishing, so tags do not publish desktop releases.

## License and attribution

This source is based on Telegram Desktop and retains its GNU GPL v3 license with the OpenSSL exception. See [LICENSE](LICENSE) for the license terms that apply to the upstream code.
