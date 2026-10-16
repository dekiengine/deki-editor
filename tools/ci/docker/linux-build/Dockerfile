# The Linux build environment for CI and the release: Ubuntu 24.04, the system
# the release has always been built on (so its binaries run wherever they did),
# with GCC 16 from the toolchain PPA and every package the Linux jobs install.
#
# Built by .github/workflows/build-image.yml and pushed to
# ghcr.io/dekiengine/deki-linux-build. The jobs run inside it instead of
# installing on every run: the PPA's server once refused GitHub's runners for
# hours (503 on every download), which stopped CI and would have stopped a
# release. Only rebuilding this image needs the PPA now.
#
# Changing what is installed: edit this file, bump IMAGE_TAG in
# build-image.yml and the `image:` lines in ci.yml and release.yml together.

FROM ubuntu:24.04

ENV DEBIAN_FRONTEND=noninteractive

# The same script the jobs used: it adds the PPA with retries and makes apt
# retry failed downloads. It calls sudo, which this image keeps for the jobs'
# own scripts anyway.
COPY add_toolchain_ppa.sh /tmp/add_toolchain_ppa.sh

RUN apt-get update \
 && apt-get install -y --no-install-recommends ca-certificates sudo software-properties-common gpg-agent \
 && bash /tmp/add_toolchain_ppa.sh \
 && apt-get update \
 && apt-get install -y --no-install-recommends \
      g++-16 gcc-16 cmake ninja-build ccache git curl pkg-config patchelf xvfb \
      python3 python3-venv libusb-1.0-0 openssh-client xz-utils unzip zstd file \
      libx11-dev libxext-dev libxrandr-dev libxcursor-dev libxi-dev \
      libxss-dev libxkbcommon-dev libwayland-dev libdecor-0-dev \
      libegl1-mesa-dev libgl1-mesa-dev libasound2-dev libpulse-dev \
      libdbus-1-dev libudev-dev \
 && rm -rf /var/lib/apt/lists/* /tmp/add_toolchain_ppa.sh

# The workspace a job mounts is owned by another user than the one the steps
# run as, and git refuses such a repository ("dubious ownership"). The engine
# submodule checkout runs plain git commands on it.
RUN git config --system --add safe.directory '*'
