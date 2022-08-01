Fork from <https://github.com/edgelesssys/edgelessdb> with some modifications.

## Changes

1. bigger HEAPSIZE (8GB)
2. private key at `./private.pem`
3. production ON
4. maxium NumTCS (1024)

```sh
# prepare private key
openssl genrsa -out private.pem -3 3072

# build
docker build . -t ppcelery/edgelessdb:8G
```

MRSIGNER & MRENCLAVE will be printed during build:

![build](https://s3.laisky.com/uploads/2022/08/edb-build.png)
