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

The `MacOS.` workflow is the required pull request gate for changes targeting `dev`. It builds the arm64 client from the pull request merge head and checks that the app launches. Its nightly run on `dev` also replays the full chat info session regression under LLDB. `MacOS Packaged.` builds manual QA artifacts on dispatch and publishes signed releases on `dev` and `main` pushes. It is not a merge gate. `MacOS unit feedback.` remains manually dispatchable and is not a merge gate. Linux GUI CI is not a gate.

## License and attribution

This source is based on Telegram Desktop and retains its GNU GPL v3 license with the OpenSSL exception. See [LICENSE](LICENSE) for the license terms that apply to the upstream code.
