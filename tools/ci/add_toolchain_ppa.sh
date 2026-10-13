#!/usr/bin/env bash
# Add the Ubuntu toolchain PPA, which carries GCC 16, retrying on failure.
#
#     bash tools/ci/add_toolchain_ppa.sh
#
# add-apt-repository asks Launchpad's API for the PPA's signing key, and that
# API times out now and then (TimeoutError: [Errno 110]); a nightly job should
# not fail on it. Three tries, 30 s apart, then the last error stands.
set -u

for attempt in 1 2 3; do
    if sudo add-apt-repository -y ppa:ubuntu-toolchain-r/test; then
        exit 0
    fi
    if [ "$attempt" -lt 3 ]; then
        echo "add-apt-repository failed (attempt $attempt of 3); retrying in 30 s"
        sleep 30
    fi
done
echo "::error::Could not add ppa:ubuntu-toolchain-r/test after 3 attempts"
exit 1
