#!/bin/bash -ex
export UBUNTU=16.04
export DOCKER_BUILDKIT=1
docker build -f build.ubuntu-${UBUNTU}.Dockerfile .
IMAGE_ID=$(docker build -q -f build.ubuntu-${UBUNTU}.Dockerfile .)

docker run --rm --user $(id -u):$(id -g) -it -v $(pwd):/app ${IMAGE_ID} python3 buildscripts/scons.py \
  --install-mode=hygienic \
  --disable-warnings-as-errors \
  --dbg=off \
  --separate-debug=on \
  --link-model=static \
  MONGO_VERSION=4.4.4-patch1 \
  LINKFLAGS="-static-libgcc -static-libstdc++ -L/usr/lib/gcc/x86_64-linux-gnu/8" \
  DESTDIR=/app/build/tar/mongodb-linux-${UBUNTU} \
  install-core

(
    cd build/tar/mongodb-linux-${UBUNTU}
    touch bin/install_compass
)

tar -czv -f build/mongodb-linux-${UBUNTU}.tar.gz -C build/tar mongodb-linux-${UBUNTU}

cat << EOF | docker run --rm -i -v $(pwd):/app -v $(pwd)/build/repo:/repo ${IMAGE_ID}
#!/bin/bash -ex
cd buildscripts
python3 packager.py -s 4.4.4-patch1 -t /app/build/mongodb-linux-${UBUNTU}.tar.gz -d ubuntu${UBUNTU/./} -a x86_64
mkdir -p /app/build/ubuntu-${UBUNTU}
find /tmp -iname '*.deb' -exec mv -f {} /app/build/ubuntu-${UBUNTU}/ \;
EOF
