FROM ubuntu:24.04@sha256:c4a8d5503dfb2a3eb8ab5f807da5bc69a85730fb49b5cfca2330194ebcc41c7b AS apt-snapshot-bootstrap

ARG DEBIAN_FRONTEND=noninteractive
ARG UBUNTU_SNAPSHOT=20260801T000000Z
ARG CA_CERTIFICATES_VERSION=20260601~24.04.1
ARG OPENSSL_VERSION=3.0.13-0ubuntu3.12
ARG QT_VERSION=6.8.3
ARG AQT_VERSION=3.3.0
ARG USERNAME=ubuntu
ARG USER_UID=1000
ARG USER_GID=1000

COPY .devcontainer/ubuntu-snapshot.sources.list \
    /usr/local/share/goldencheetah/ubuntu-snapshot.sources.list
COPY appveyor/linux/verify-apt-snapshot.py /usr/local/libexec/verify-apt-snapshot.py
COPY appveyor/linux/verify-apt-snapshot-bootstrap.sh \
    /usr/local/libexec/verify-apt-snapshot-bootstrap.sh
ADD --checksum=sha256:6bac2a01979e210d9eac1d4d56747ec709ea60654744d66705dc3c36e7629e50 \
    https://snapshot.ubuntu.com/ubuntu/20260801T000000Z/pool/main/c/ca-certificates/ca-certificates_20260601~24.04.1_all.deb \
    /tmp/ca-certificates-bootstrap.deb

SHELL ["/bin/bash", "-o", "pipefail", "-c"]

RUN printf '%s  %s\n' \
        '6bac2a01979e210d9eac1d4d56747ec709ea60654744d66705dc3c36e7629e50' \
        /tmp/ca-certificates-bootstrap.deb \
        | sha256sum --check --strict \
    && rm -rf /tmp/ca-certificates-bootstrap \
    && dpkg-deb --extract /tmp/ca-certificates-bootstrap.deb \
        /tmp/ca-certificates-bootstrap \
    && install -d -m 0755 /etc/ssl/certs \
    && find /tmp/ca-certificates-bootstrap/usr/share/ca-certificates \
        -type f -name '*.crt' -print0 \
        | sort -z | xargs -0 cat \
        > /etc/ssl/certs/ca-certificates.crt \
    && test -s /etc/ssl/certs/ca-certificates.crt \
    && find /etc/apt/sources.list.d -mindepth 1 -maxdepth 1 \
        -exec rm -rf -- {} + \
    && install -m 0644 /usr/local/share/goldencheetah/ubuntu-snapshot.sources.list \
        /etc/apt/sources.list \
    && printf 'APT::Snapshot "%s";\n' "${UBUNTU_SNAPSHOT}" \
        >/etc/apt/apt.conf.d/50goldencheetah-snapshot \
    && rm -rf /var/lib/apt/lists/* \
    && apt-get update --error-on=any \
        -o Acquire::AllowInsecureRepositories=false \
        -o Acquire::AllowDowngradeToInsecureRepositories=false \
    && find /var/lib/apt/lists -maxdepth 1 \
        \( -name '*_InRelease' -o -name '*_Packages*' \) \
        ! -name "snapshot.ubuntu.com_ubuntu_${UBUNTU_SNAPSHOT}_*" -delete \
    && APT_VERSION=$(apt-get --version | sed -n '1s/^apt \([^ ]*\).*/\1/p') \
    && /usr/local/libexec/verify-apt-snapshot-bootstrap.sh \
        /etc/apt/sources.list /var/lib/apt/lists "${UBUNTU_SNAPSHOT}" \
        "${APT_VERSION}" noble amd64 \
    && apt-get install --no-install-recommends -y \
        -o Acquire::AllowInsecureRepositories=false \
        -o Acquire::AllowDowngradeToInsecureRepositories=false \
        -o APT::Get::AllowUnauthenticated=false \
        ca-certificates="${CA_CERTIFICATES_VERSION}" \
        libssl3t64="${OPENSSL_VERSION}" \
        openssl="${OPENSSL_VERSION}" \
    && test "$(dpkg-query -W -f='${Version}' ca-certificates)" \
        = "${CA_CERTIFICATES_VERSION}" \
    && test "$(dpkg-query -W -f='${Version}' libssl3t64)" \
        = "${OPENSSL_VERSION}" \
    && test "$(dpkg-query -W -f='${Version}' openssl)" \
        = "${OPENSSL_VERSION}" \
    && test -s /etc/ssl/certs/ca-certificates.crt \
    && rm -rf /tmp/ca-certificates-bootstrap \
        /tmp/ca-certificates-bootstrap.deb \
    && apt-get install --no-install-recommends -y \
        -o Acquire::AllowInsecureRepositories=false \
        -o Acquire::AllowDowngradeToInsecureRepositories=false \
        -o APT::Get::AllowUnauthenticated=false \
        autoconf \
        automake \
        bison \
        build-essential \
        ccache \
        cmake \
        curl \
        flex \
        fonts-dejavu-core \
        gdb \
        git \
        libasound2t64 \
        libatspi2.0-0 \
        libbluetooth-dev \
        libcap2 \
        libcups2t64 \
        libdbus-1-3 \
        libdbus-1-dev \
        libdrm2 \
        libegl1-mesa-dev \
        libgbm1 \
        libgl1-mesa-dev \
        libglib2.0-0 \
        libglu1-mesa-dev \
        libgnutls30t64 \
        libgsl-dev \
        libgtk-3-0 \
        libtool \
        libnss3 \
        libpulse0 \
        libudev-dev \
        libx11-xcb1 \
        libxcb-cursor0 \
        libxcomposite1 \
        libxdamage1 \
        libxi6 \
        libxkbcommon-x11-0 \
        libxrandr2 \
        libxrender1 \
        libxss1 \
        libxtst6 \
        make \
        ninja-build \
        p7zip-full \
        pkg-config \
        python3 \
        python3-pip \
        python3-venv \
        sudo \
        unzip \
    && APT_VERSION=$(apt-get --version | sed -n '1s/^apt \([^ ]*\).*/\1/p') \
    && python3 /usr/local/libexec/verify-apt-snapshot.py \
        --sources /etc/apt/sources.list --lists /var/lib/apt/lists \
        --snapshot "${UBUNTU_SNAPSHOT}" --apt-version "${APT_VERSION}" \
        --series noble --architecture amd64 \
    && rm -rf /var/lib/apt/lists/*

FROM apt-snapshot-bootstrap AS goldencheetah-development

ARG DEBIAN_FRONTEND=noninteractive
ARG UBUNTU_SNAPSHOT=20260801T000000Z
ARG QT_VERSION=6.8.3
ARG AQT_VERSION=3.3.0
ARG USERNAME=ubuntu
ARG USER_UID=1000
ARG USER_GID=1000

COPY .devcontainer/aqt-requirements.lock /tmp/aqt-requirements.lock
COPY .devcontainer/qt-6.8.3-linux-gcc64.lock /tmp/qt-archives.lock
COPY .devcontainer/install-verified-qt.py /usr/local/libexec/install-verified-qt.py

RUN python3 -m venv /opt/aqt \
    && test "$(dpkg --print-architecture)" = amd64 \
    && PIP_CONFIG_FILE=/dev/null /opt/aqt/bin/python -m pip install \
        --isolated --disable-pip-version-check --no-input --no-cache-dir \
        --require-hashes --only-binary=:all: \
        --index-url=https://pypi.org/simple \
        -r /tmp/aqt-requirements.lock \
    && test "$(/opt/aqt/bin/python -c \
        'from importlib.metadata import version; print(version("aqtinstall"))')" \
        = "${AQT_VERSION}" \
    && /opt/aqt/bin/python /usr/local/libexec/install-verified-qt.py \
        --lock /tmp/qt-archives.lock --output /opt/Qt \
        --base https://download.qt.io --version "${QT_VERSION}" \
        --arch linux_gcc_64 --expected-count 15 \
        --module qt5compat --module qtcharts --module qtconnectivity \
        --module qtmultimedia --module qtpositioning --module qtserialport \
        --module qtwebchannel --module qtwebengine \
    && rm -f /aqtinstall.log /tmp/aqt-requirements.lock \
        /tmp/qt-archives.lock

RUN apt-get update --error-on=any \
        -o Acquire::AllowInsecureRepositories=false \
        -o Acquire::AllowDowngradeToInsecureRepositories=false \
    && find /var/lib/apt/lists -maxdepth 1 \
        \( -name '*_InRelease' -o -name '*_Packages*' \) \
        ! -name "snapshot.ubuntu.com_ubuntu_${UBUNTU_SNAPSHOT}_*" -delete \
    && APT_VERSION=$(apt-get --version | sed -n '1s/^apt \([^ ]*\).*/\1/p') \
    && python3 /usr/local/libexec/verify-apt-snapshot.py \
        --sources /etc/apt/sources.list --lists /var/lib/apt/lists \
        --snapshot "${UBUNTU_SNAPSHOT}" --apt-version "${APT_VERSION}" \
        --series noble --architecture amd64 \
    && apt-get install --no-install-recommends -y \
        -o Acquire::AllowInsecureRepositories=false \
        -o Acquire::AllowDowngradeToInsecureRepositories=false \
        -o APT::Get::AllowUnauthenticated=false \
        libsecret-1-dev \
        libxcb-icccm4 \
        libxcb-keysyms1 \
        libxcb-shape0 \
        libxkbfile-dev \
        file \
        patchelf \
        python3-dev \
        squashfs-tools \
        zlib1g-dev

RUN echo "${USERNAME} ALL=(root) NOPASSWD:ALL" > "/etc/sudoers.d/${USERNAME}" \
    && chmod 0440 "/etc/sudoers.d/${USERNAME}" \
    && mkdir -p /workspace \
    && chown "${USERNAME}:${USERNAME}" /workspace

ENV QT_VERSION="${QT_VERSION}"
ENV QTDIR="/opt/Qt/${QT_VERSION}/gcc_64"
ENV PATH="/opt/Qt/${QT_VERSION}/gcc_64/bin:${PATH}"
ENV LD_LIBRARY_PATH="/opt/Qt/${QT_VERSION}/gcc_64/lib"
ENV QTWEBENGINE_DISABLE_SANDBOX=1

USER ${USERNAME}
WORKDIR /workspace

CMD ["bash"]
