# Railway benchmark image

The root `Dockerfile` builds this Git fork with `USE_S3=YesPlease` and installs it in a small Debian runtime image. `/app/bin/cloud-bench` forwards benchmark commands to `git cloud-bench`.

The wrapper handles `serve` itself for now. It creates one bare repository and exposes it through Git's stock `git-http-backend`. The default URL is `/bench.git`; `/healthz` returns `{"status":"ok"}`. This is a protocol test server over the container filesystem, not an S3 object database.

Set `CLOUD_BENCH_REPO_ROOT` to an absolute repository directory and `CLOUD_BENCH_REPO_NAME` to change the served path. Anonymous pushes are disabled by default. Set `CLOUD_BENCH_ALLOW_PUSH=1` only in an isolated benchmark environment that needs push tests.

Build and smoke-test the image:

```sh
docker build -t scope-git-cloud-bench .
docker run --rm -p 8080:8080 scope-git-cloud-bench
curl --fail http://127.0.0.1:8080/healthz
git ls-remote http://127.0.0.1:8080/bench.git
contrib/cloud-bench/smoke-http.sh
```

Deploy from the repository root, then run benchmark commands inside the service:

```sh
railway up --service git-cloud-bench --detach -m "Deploy cloud benchmark"
railway deployment list --service git-cloud-bench --json
railway ssh --service git-cloud-bench -- /app/bin/cloud-bench probe --json
```

`probe` validates immutable writes, exact range reads, ETag stability,
conditional manifest replacement, restart visibility, and a two-publisher CAS
race against the configured bucket. It does not run the plan's three-layout
cloud writer matrix. The local artifact seam for those layouts is covered by
`t/t5353-storage-layout-benchmark.sh` and labels its output with
`"cloudMeasured": false`.

The service filesystem is ephemeral. Redirect JSON output to a local file or
another durable destination before redeploying or deleting the service. A
successful capability result from the initial Railway bucket is checked in at
`contrib/cloud-bench/results/railway-s3-probe-2026-08-20.json`.
