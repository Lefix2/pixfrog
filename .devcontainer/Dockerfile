# Same image as the CI idf-build job (.github/workflows/ci.yml) so a local
# `idf.py build` is directly comparable with CI.
FROM espressif/idf:v5.5

# Tooling for the other CI jobs + repo host tools:
#  - clang-format 18 (pip, pinned) == what ubuntu-latest installs in the
#    format-check job; the image's apt only carries an older major.
#  - build-essential + cmake for the three host test suites.
#  - libsdl2-dev for tools/emulator.
RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential \
        cmake \
        libsdl2-dev \
        python3-pip \
    && rm -rf /var/lib/apt/lists/* \
    && pip3 install --no-cache-dir clang-format==18.1.8

# The image only sources the IDF env via its docker entrypoint, which VS Code
# bypasses — make every shell an IDF shell.
RUN echo '. "$IDF_PATH/export.sh" >/dev/null 2>&1' >> /root/.bashrc
