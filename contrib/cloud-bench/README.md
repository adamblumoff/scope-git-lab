# Railway benchmark image

The root `Dockerfile` builds this Git fork with `USE_S3=YesPlease` and installs it in a small Debian runtime image. `/app/bin/cloud-bench` forwards benchmark commands to `git cloud-bench`.

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
credentials are present; set it to `0` only for the local HTTP smoke test.

The bridge defaults to 64 concurrent requests, a 30-second total request-input
deadline and socket inactivity timeout, 1 GiB per request, and 1 GiB total
buffered request data. Override these with `CLOUD_BENCH_MAX_CONCURRENT_REQUESTS`,
`CLOUD_BENCH_REQUEST_TIMEOUT_SECONDS`, `CLOUD_BENCH_MAX_REQUEST_BYTES`, and
`CLOUD_BENCH_MAX_BUFFERED_REQUEST_BYTES`. Receive-pack status responses are
spooled up to 16 MiB so a crashed backend can return a terminal HTTP error;
override that bound with `CLOUD_BENCH_MAX_RECEIVE_RESPONSE_BYTES`. Admission
fails with HTTP 503 when a server-wide limit is full.

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
payload size. Each writer level is followed by a fresh mirror clone and strict
fsck; the default run also injects five receive-pack crash points. Results use
the `git-cloud-odb-matrix/v1` schema and are written to `/results/latest.json`
as well as stdout.

The local artifact seam remains covered by
`t/t5353-storage-layout-benchmark.sh`; its JSON is explicitly labeled
`"cloudMeasured": false`.

The service filesystem is ephemeral. Redirect JSON output to a local file or
another durable destination before redeploying or deleting the service. The
checked-in results directory contains the successful Railway bucket probe, the
15-boundary failure run, and the raw three-layout decision matrix.
