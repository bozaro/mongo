ARG UBUNTU=18.04

FROM ubuntu:$UBUNTU
RUN apt update && \
    apt install -y \
      apt-utils \
      debhelper \
      build-essential \
      git \
      libcurl4-openssl-dev \
      liblzma-dev \
      libssl-dev \
      software-properties-common

WORKDIR /app

RUN add-apt-repository ppa:ubuntu-toolchain-r/test
RUN apt update
RUN apt install -y gcc-8 g++-8
RUN update-alternatives --install /usr/bin/gcc gcc /usr/bin/gcc-8 60 --slave /usr/bin/g++ g++ /usr/bin/g++-8

RUN apt update && \
    apt install -y \
      python3-pip

COPY etc/pip etc/pip
RUN python3 -m pip install -U -r etc/pip/compile-requirements.txt
