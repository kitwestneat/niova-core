FROM ubuntu:18.04

RUN apt-get update -y \
    && apt-get -y install libaio-dev openssl libk5crypto3 librocksdb5.8 uuid vim python3-pip \
    && pip3 install func_timeout sockets psutil dpath \
    && pip3 install ansible \
    && pip3 install jmespath
WORKDIR /opt
COPY holon /opt/bin/
RUN mkdir -p /opt/sbin/niova/
RUN mkdir /opt/bin

COPY pumicedb-server-test /opt/sbin/niova/
COPY pumicedb-client-test /opt/sbin/niova/
COPY raft-server /opt/sbin/niova/
COPY raft-client /opt/sbin/niova/

RUN ls -l /opt/bin/
RUN ls -l /opt/sbin/niova
