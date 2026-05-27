#!/bin/sh
# dodoc installer — https://getdodo.dev
# Usage: curl -fsSL https://getdodo.dev/install.sh | sh
set -e

REPO="codedthinking/dodo"
BINARY="dodoc"

main() {
    os="$(uname -s)"
    arch="$(uname -m)"

    case "$os" in
        Darwin)  os_label="macos" ;;
        Linux)   os_label="linux" ;;
        MINGW*|MSYS*|CYGWIN*)
            echo "error: Windows detected. Download dodoc from:"
            echo "  https://github.com/$REPO/releases/latest"
            exit 1
            ;;
        *)
            echo "error: unsupported OS: $os"
            exit 1
            ;;
    esac

    case "$arch" in
        x86_64|amd64)  arch_label="x86_64" ;;
        arm64|aarch64) arch_label="arm64" ;;
        *)
            echo "error: unsupported architecture: $arch"
            exit 1
            ;;
    esac

    artifact="dodoc-${os_label}-${arch_label}"
    echo "detected: ${os_label}/${arch_label}"

    # Find the latest release download URL
    release_url="https://api.github.com/repos/$REPO/releases/latest"
    if command -v curl > /dev/null 2>&1; then
        download_url=$(curl -fsSL "$release_url" | grep "browser_download_url.*${artifact}.tar.gz" | head -1 | cut -d '"' -f 4)
    elif command -v wget > /dev/null 2>&1; then
        download_url=$(wget -qO- "$release_url" | grep "browser_download_url.*${artifact}.tar.gz" | head -1 | cut -d '"' -f 4)
    else
        echo "error: curl or wget required"
        exit 1
    fi

    if [ -z "$download_url" ]; then
        echo "error: no binary found for ${artifact}"
        echo "check available releases at: https://github.com/$REPO/releases"
        exit 1
    fi

    echo "downloading: $download_url"

    tmpdir="$(mktemp -d)"
    trap 'rm -rf "$tmpdir"' EXIT

    if command -v curl > /dev/null 2>&1; then
        curl -fsSL "$download_url" -o "$tmpdir/${artifact}.tar.gz"
    else
        wget -qO "$tmpdir/${artifact}.tar.gz" "$download_url"
    fi

    tar xzf "$tmpdir/${artifact}.tar.gz" -C "$tmpdir"

    # Remove macOS quarantine attribute (blocks unsigned binaries)
    if [ "$os" = "Darwin" ]; then
        xattr -d com.apple.quarantine "$tmpdir/$BINARY" 2>/dev/null || true
    fi

    # Pick install dir: /opt/homebrew/bin on ARM Mac, /usr/local/bin otherwise, ~/.local/bin as fallback
    if [ "$os" = "Darwin" ] && [ "$arch" = "arm64" ] && [ -d "/opt/homebrew/bin" ]; then
        install_dir="/opt/homebrew/bin"
    else
        install_dir="/usr/local/bin"
    fi
    if [ -w "$install_dir" ]; then
        install "$tmpdir/$BINARY" "$install_dir/$BINARY"
    elif command -v sudo > /dev/null 2>&1; then
        echo "installing to $install_dir (requires sudo)"
        sudo install "$tmpdir/$BINARY" "$install_dir/$BINARY"
    else
        install_dir="$HOME/.local/bin"
        mkdir -p "$install_dir"
        install "$tmpdir/$BINARY" "$install_dir/$BINARY"
    fi

    echo "installed: $("$install_dir/$BINARY" --help 2>&1 | head -1)"
    echo "dodoc is ready at $install_dir/$BINARY"

    case ":$PATH:" in
        *":$install_dir:"*) ;;
        *) echo "note: $install_dir is not in your PATH — add it to your shell profile" ;;
    esac
}

main
