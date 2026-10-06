FROM gcc:14-bookworm AS build
WORKDIR /app
COPY Makefile ./
COPY src/ ./src/
RUN make server

FROM debian:bookworm-slim
WORKDIR /app
COPY --from=build /app/build/server ./subnet-server
EXPOSE 9666
CMD ["./subnet-server"]
