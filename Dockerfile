FROM ubuntu:22.04

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y \
    build-essential \
    gcc \
    gdb \
    make \
    cmake \
    valgrind \
    rsync \
    tar \
    && rm -rf /var/lib/apt/lists/*