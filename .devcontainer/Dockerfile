FROM ubuntu:24.04

ARG DEBIAN_FRONTEND=noninteractive
ARG QT_VERSION=6.8.3
ARG AQT_VERSION=3.3.0
ARG USERNAME=ubuntu
ARG USER_UID=1000
ARG USER_GID=1000

RUN apt-get update \
    && apt-get install --no-install-recommends -y \
        bison \
        build-essential \
        ca-certificates \
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
        libcups2t64 \
        libdbus-1-3 \
        libdbus-1-dev \
        libdrm2 \
        libegl1-mesa-dev \
        libgbm1 \
        libgl1-mesa-dev \
        libglib2.0-0 \
        libglu1-mesa-dev \
        libgsl-dev \
        libgtk-3-0 \
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
    && rm -rf /var/lib/apt/lists/*

RUN python3 -m venv /opt/aqt \
    && /opt/aqt/bin/pip install --no-cache-dir "aqtinstall==${AQT_VERSION}" \
    && /opt/aqt/bin/aqt install-qt linux desktop "${QT_VERSION}" linux_gcc_64 \
        -O /opt/Qt \
        -m qt5compat qtcharts qtconnectivity qtmultimedia qtpositioning \
           qtserialport qtwebchannel qtwebengine

RUN apt-get update \
    && apt-get install --no-install-recommends -y \
        libxcb-icccm4 \
        libxcb-keysyms1 \
        libxcb-shape0 \
        libxkbfile-dev \
        file \
        patchelf \
        python3-dev \
        zlib1g-dev \
    && rm -rf /var/lib/apt/lists/*

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
