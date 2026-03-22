FROM postgres:16-alpine

COPY sql/qjson_pg.sql /docker-entrypoint-initdb.d/01-qjson.sql

# Data checksums for integrity verification
ENV POSTGRES_INITDB_ARGS="--data-checksums"
