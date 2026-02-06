FROM alpine:latest

# Install dependencies
RUN apk add --no-cache curl openssl

# Layer 1: Create 100MB random data
RUN dd if=/dev/urandom of=/layer1.bin bs=1M count=100 && \
    ls -lh /layer1.bin

# Layer 2: Create 100MB random data with different content
RUN openssl rand -out /layer2.bin 104857600 && \
    ls -lh /layer2.bin

# Layer 3: Create 150MB random data
RUN dd if=/dev/urandom of=/layer3.bin bs=1M count=150 && \
    ls -lh /layer3.bin

# Layer 4: Create multiple random files
RUN for i in {1..10}; do \
      dd if=/dev/urandom of=/data_file_$i.bin bs=1M count=10; \
    done && \
    ls -lh /data_file_*.bin

# Layer 5: Copy and process cache version
COPY ./cache-version.txt /cache-version.txt
RUN cat /cache-version.txt && \
    dd if=/dev/urandom of=/layer5.bin bs=1M count=75
