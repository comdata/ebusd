FROM debian:stretch-slim

RUN apt-get update && apt-get install -y \
    logrotate libmosquitto1 libstdc++6 libc6 libgcc1 \
    curl \
    && rm -rf /var/lib/apt/lists/*

RUN apt-get update && apt-get install -y \
    autoconf automake g++ make git \
    libmosquitto-dev \
    && rm -rf /var/lib/apt/lists/*

LABEL maintainer "ebusd@ebusd.eu"

ENV EBUSD_VERSION 21.1
ENV EBUSD_ARCH armhf

LABEL version "${EBUSD_VERSION}-${EBUSD_ARCH}-devel"

WORKDIR /build

RUN (curl -SL https://github.com/john30/ebusd/archive/master.tar.gz \
    | tar xz --strip-components=1) \
    && ./autogen.sh \
    && make install-strip

WORKDIR /

RUN rm -rf /build

EXPOSE 8888

COPY release/docker-entrypoint.sh /
ENTRYPOINT ["/docker-entrypoint.sh"]
CMD ["-f", "--scanconfig"]
