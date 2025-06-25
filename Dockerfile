FROM ubuntu:22.04

RUN apt-get update && apt-get install -y g++ make

WORKDIR /app

# Copy all source files
COPY Makefile proxy_parse.c proxy_parse.h proxy_server.cpp /app

# Build the proxy using your Makefile
RUN make

# Expose the proxy port
EXPOSE 8080

# Run the compiled proxy binary
CMD ["./proxy", "8080"]