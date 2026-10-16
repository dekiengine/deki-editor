#!/usr/bin/env bash
# Add the Ubuntu toolchain PPA, which carries GCC 16, retrying on failure.
#
#     bash tools/ci/add_toolchain_ppa.sh
#
# add-apt-repository asks Launchpad's API for the PPA's signing key, and that
# API times out now and then (TimeoutError: [Errno 110]); a nightly job should
# not fail on it. Three tries, 30 s apart, then the last error stands.
set -u

# The PPA's file server answers some downloads with 503 Service Unavailable
# under load, and the ~100 packages of GCC 16 and its runtime meet a few of
# them; apt gave up on the first and failed the job. It now retries each
# failed download (growing delays between tries) for every apt-get after this.
echo 'Acquire::Retries "10";' | sudo tee /etc/apt/apt.conf.d/80-deki-retries >/dev/null

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
