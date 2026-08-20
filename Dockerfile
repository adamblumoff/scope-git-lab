# syntax=docker/dockerfile:1

FROM debian:bookworm-slim AS build

ARG MAKE_JOBS=2

RUN apt-get update \
	&& DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
		build-essential \
		ca-certificates \
		libcurl4-openssl-dev \
		libexpat1-dev \
		libssl-dev \
		time \
		zlib1g-dev \
	&& rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY . .

RUN make clean \
	&& make -j"${MAKE_JOBS}" \
		USE_S3=YesPlease \
		NO_GETTEXT=YesPlease \
		NO_PERL=YesPlease \
		NO_RUST=UnfortunatelyYes \
		NO_TCLTK=YesPlease \
		prefix=/usr/local \
		all \
	&& make \
		USE_S3=YesPlease \
		NO_GETTEXT=YesPlease \
		NO_PERL=YesPlease \
		NO_RUST=UnfortunatelyYes \
		NO_TCLTK=YesPlease \
		prefix=/usr/local \
		DESTDIR=/opt/git \
		install

FROM debian:bookworm-slim AS runtime

RUN apt-get update \
	&& DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
		ca-certificates \
		libcurl4 \
		libexpat1 \
		python3 \
		zlib1g \
	&& rm -rf /var/lib/apt/lists/*

COPY --from=build /opt/git/ /
COPY contrib/cloud-bench/cloud-bench contrib/cloud-bench/http-server.py /app/bin/

RUN chmod 0755 /app/bin/cloud-bench /app/bin/http-server.py

WORKDIR /app
ENV PATH="/usr/local/bin:/usr/local/libexec/git-core:${PATH}" \
	PORT=8080

EXPOSE 8080

CMD ["/app/bin/cloud-bench", "serve"]
