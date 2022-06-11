FROM nvidia/opencl:devel-ubuntu18.04

LABEL maintainer="radiantblockchain@protonmail.com"
LABEL version="1.0"
LABEL description="Custom docker image for rad-bfgminer"

ARG DEBIAN_FRONTEND=nointeractive

RUN apt update

RUN apt-get install -y nodejs
RUN apt-get install -y curl

RUN curl -sL https://deb.nodesource.com/setup_12.x | bash -

ENV PACKAGES="\
  build-essential \
  libcurl4-openssl-dev \
  software-properties-common \
  ubuntu-drivers-common \
  pkg-config \
  libtool \
  ocl-icd-* \
  opencl-headers \
  openssh-server \
  ocl-icd-opencl-dev\
  git \
  clinfo \
  autoconf \
  automake \
  libjansson-dev \
  libevent-dev \
  uthash-dev \
  nodejs \
  vim \
"

RUN apt update && apt install --no-install-recommends -y $PACKAGES  && \
    rm -rf /var/lib/apt/lists/* && \
    apt clean
 
RUN git clone https://github.com/radiantblockchain/rad-bfgminer.git /root/rad-bfgminer
WORKDIR /root/rad-bfgminer
RUN git config --global url.https://github.com/.insteadOf git://github.com/
RUN ./autogen.sh
RUN ./configure --enable-opencl
RUN make

#WORKDIR /root/rad-bfgminer/minerscript-js
#RUN npm install

#CMD ["/usr/sbin/ssgd", "-D"]
CMD ["bash"]
#ENTRYPOINT ["tail", "-f", "/dev/null"]