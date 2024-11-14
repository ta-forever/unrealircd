FROM debian:stable-slim

ENV VERSION=6.1.8.1
ENV TZ=Europe/London
ENV LANG en_GB.UTF-8
ENV LANGUAGE en_GB:en
ENV LC_ALL en_GB.UTF-8

ARG DEBIAN_FRONTEND=noninteractive

RUN ln -snf /usr/share/zoneinfo/$TZ /etc/localtime && \
    echo $TZ > /etc/timezone && \
    apt-get -qq update && \
    apt-get -yqq --no-install-recommends install \
        locales curl openssl build-essential pkg-config libssl-dev libpcre2-dev \
        libargon2-0-dev libsodium-dev libc-ares-dev libcurl4-openssl-dev && \
    sed -i -e 's/# en_GB.UTF-8 UTF-8/en_GB.UTF-8 UTF-8/' /etc/locale.gen && \
    dpkg-reconfigure --frontend=noninteractive locales && \
    update-locale LANG=en_GB.UTF-8

RUN groupadd -r unrealircd && \
    useradd -rm -s /bin/false -g unrealircd unrealircd && \
    mkdir /install && \
    mkdir /unrealircd && \
    curl -ksL https://www.unrealircd.org/downloads/unrealircd-${VERSION}.tar.gz -o unrealircd-${VERSION}.tar.gz && \
    tar zxf unrealircd-${VERSION}.tar.gz -C /install --strip-components 1 && \
    chown -R unrealircd:unrealircd /install && \
    chown -R unrealircd:unrealircd /unrealircd

COPY --chown=unrealircd:unrealircd config.settings /install
COPY --chown=unrealircd:unrealircd src/ /install/src/

WORKDIR /install
USER unrealircd
RUN ./Config && \
    make && \
    make install

# clean up
WORKDIR /
USER root
RUN apt-get -yqq remove --purge \
        curl openssl build-essential pkg-config libssl-dev libpcre2-dev \
        libargon2-0-dev libsodium-dev libc-ares-dev libcurl4-openssl-dev && \
    apt-get -yqq autoremove --purge && \
    rm -rf /install && \
    rm -f /unrealircd/source && \
    rm -f /unrealircd-${VERSION}.tar.gz

RUN apt-get -yqq install libcurl4 libc-ares2 libsodium23 ca-certificates

WORKDIR /unrealircd
USER unrealircd
CMD ["/unrealircd/bin/unrealircd", "-F" ]
