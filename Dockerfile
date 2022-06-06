FROM nvidia/opencl:devel-ubuntu18.04

LABEL maintainer="radiantblockchain@protonmail.com"
LABEL version="1.0"
LABEL description="Custom docker image for rad-bfgminer"

ARG DEBIAN_FRONTEND=nointeractive

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
"

RUN apt update && apt install --no-install-recommends -y $PACKAGES  && \
    rm -rf /var/lib/apt/lists/* && \
    apt clean

# TODO implement CGMINER at future point
# Currently supports Novo
RUN git clone https://github.com/Bit90pool/novo-cgminer.git /root/novo-cgminer
WORKDIR /root/novo-cgminer
RUN chmod +x auto_compile.sh && \
    ./auto_compile.sh
 
RUN git clone https://github.com/radiantblockchain/rad-bfgminer.git /root/rad-bfgminer
WORKDIR /root/rad-bfgminer
RUN autogen.sh && configure --enable-opencl && make


# ./bfgminer -S opencl:auto -o http://node.radiantblockchain.org:7332 -u raduser -p radpass



#CMD ["/usr/sbin/ssgd", "-D"]
CMD ["bash"]
#ENTRYPOINT ["tail", "-f", "/dev/null"]