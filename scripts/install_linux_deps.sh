#!/bin/bash
# Linux dependencies for Ruzino Framework
# Run this script with sudo: sudo ./scripts/install_linux_deps.sh

set -e

echo "Installing Linux development dependencies for Ruzino..."

# Update package lists
apt-get update

# X11 and OpenGL development libraries (required for OpenUSD/MaterialX)
apt install -y \
    libx11-dev \
    libxrandr-dev \
    libxinerama-dev \
    libxcursor-dev \
    libxi-dev \
    libxxf86vm-dev \
    libxt-dev \
    libxmu-dev \
    libgl1-mesa-dev

# Build tools (required for OpenUSD compilation)
apt install -y \
    build-essential \
    cmake \
    ninja-build \
    git \
    curl \
    wget

# Python development (for OpenUSD Python bindings)
apt install -y \
    python3 \
    python3-dev \
    python3-pip \
    python3-venv

# Additional libraries that may be needed
apt install -y \
    libtbb-dev \
    libboost-all-dev \
    libzstd-dev \
    libblosc-dev

echo ""
echo "✓ All dependencies installed successfully!"
echo ""
echo "Now you can run:"
echo "  python3 configure.py --all --build_variant Debug"
