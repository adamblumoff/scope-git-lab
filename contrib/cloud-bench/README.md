# Railway benchmark image

The root `Dockerfile` builds this Git fork with `USE_S3=YesPlease` and installs it in a small Debian runtime image. `/app/bin/cloud-bench` forwards benchmark commands to `git cloud-bench`.

For Meson builds, pass `-Ds3=enabled`. This opt-in feature requires libcurl
7.75.0 or later and builds both the cloud ODB sources and `git-cloud-bench`.

`serve` exposes Git's stock `git-http-backend` at `/bench.git`. Objects are
stored as bounded compressed groups with a one-group read cache, immutable
content-addressed uploads, and ETag-conditional manifest publication. Refs
remain on the container filesystem. `/healthz` returns `{"status":"ok"}`.

The decision matrix also measured remote pack byte windows and independently
compressed full objects. Pack reads failed the 50-writer verification and
restart time gates. Full-object segments passed but were slower to verify and
restart than bounded groups, so both losing cloud layouts were removed.

Set `CLOUD_BENCH_REPO_ROOT` to an absolute repository directory and
`CLOUD_BENCH_REPO_NAME` to change the served path. Anonymous pushes are disabled
by default. Set `CLOUD_BENCH_ALLOW_PUSH=1` only in an isolated benchmark
environment. `CLOUD_BENCH_CLOUD_ODB=auto` enables the cloud ODB when bucket
credentials are present; a partial AWS environment fails startup before a
repository marker is created, and complete credentials are semantically
validated before the marker is installed. Set it to `0` only for the local HTTP
smoke test. The POC rejects any canonical object larger than 16 MiB, any receive
transaction larger than 32 MiB, any publication containing more than 65,536
objects, and any generated artifact larger than 64 MiB. Those explicit limits
keep publication metadata and read streams bounded until the group codec gains
incremental object compression and inflation. Bounded direct writes use the
same publication path so objects created by receive hooks remain available.

The bridge defaults to 64 concurrent requests, a 30-second total request-input
deadline and socket inactivity timeout, 1 GiB per request, and 1 GiB total
buffered request data. Override these with `CLOUD_BENCH_MAX_CONCURRENT_REQUESTS`,
`CLOUD_BENCH_REQUEST_TIMEOUT_SECONDS`, `CLOUD_BENCH_MAX_REQUEST_BYTES`, and
`CLOUD_BENCH_MAX_BUFFERED_REQUEST_BYTES`. Receive-pack status responses are
spooled up to 16 MiB so a crashed backend can return a terminal HTTP error;
override that bound with `CLOUD_BENCH_MAX_RECEIVE_RESPONSE_BYTES`. Admission
fails with HTTP 503 when a server-wide limit is full.

`/results/latest.json` accepts results up to 64 MiB by default; override the
bound with `CLOUD_BENCH_MAX_RESULT_BYTES`. Remote failure injection is disabled
unless `CLOUD_BENCH_ALLOW_FAILPOINTS=1` is set on the isolated benchmark
service. A failpoint request header is ignored everywhere else.
The CGI bridge forwards only the allowlisted AWS, proxy, CA, TLS, and client
certificate environment needed by the S3 transport; other service secrets stay
out of `git-http-backend`.

Build and smoke-test the image:

```sh
docker build -t scope-git-cloud-bench .
docker run --rm -p 8080:8080 scope-git-cloud-bench
curl --fail http://127.0.0.1:8080/healthz
git ls-remote http://127.0.0.1:8080/bench.git
contrib/cloud-bench/smoke-http.sh
```

With bucket credentials available, the fallback smoke test verifies that an
existing files-backed repository remains fully enumerable after the cloud ODB
marker is added, including after a new cloud-backed push:

```sh
contrib/cloud-bench/smoke-cloud-fallback.sh
```

Deploy from the repository root, then run benchmark commands inside the service:

```sh
railway up --service git-cloud-bench --detach -m "Deploy cloud benchmark"
railway deployment list --service git-cloud-bench --json
railway ssh --service git-cloud-bench -- /app/bin/cloud-bench probe --json
railway ssh --service git-cloud-bench -- /app/bin/cloud-bench matrix \
  --writers 1,10,50 --warmups 1 --samples 3
```

`probe` validates immutable writes, exact range reads, ETag stability,
conditional manifest replacement, restart visibility, and a two-publisher CAS
race against the configured bucket.

`matrix` starts bounded-group writers at a barrier and records every writer
result and storage call. It accepts writer counts, warmups, sample counts, and
payload size. Use `--repository` or `CLOUD_BENCH_REPO_NAME` when the service is
not mounted at `bench.git`. Each writer level is followed by a fresh mirror
clone and strict fsck; the default run also injects five receive-pack crash
points and requires ref inspection itself to succeed. A run is labeled
cloud-measured only after its seed push produces S3 request and PUT metrics.
Each request carries a random run identifier, and the server tags every storage
event with it, so overlapping matrices can safely share the process metrics
file without attributing another run's traffic.
Results use the `git-cloud-odb-matrix/v1` schema and are written to
`/results/latest.json` as well as stdout.

The local artifact seam remains covered by
`t/t5353-storage-layout-benchmark.sh`; its JSON is explicitly labeled
`"cloudMeasured": false`.

The service filesystem is ephemeral. Redirect JSON output to a local file or
another durable destination before redeploying or deleting the service. The
checked-in results directory contains the successful Railway bucket probe, the
15-boundary failure run, and the raw three-layout decision matrix.
