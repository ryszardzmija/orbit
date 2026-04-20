# Orbit

A high-performance TCP reverse proxy written in modern C++ using Linux-native APIs. The long-term goal is to evolve this into a Dynamo-aware load balancer for a distributed key-value store.

## Architecture

The proxy accepts a downstream TCP connection and bridges it to an upstream backend. The event loop is epoll-based and runs on a single thread. Unlike poll() syscall, which scans all watched descriptors on every call, epoll() delivers only the descriptors that are ready in O(1) regardless of connection count. This allows a single thread to handle tens of thousands of concurrent connections.

epoll() is used in level-triggered mode, which means that the kernel tracks readiness state for I/O operations like reading from a socket internally instead of only notifying about state transitions (as in edge-triggered mode). This choice simplifies the proxy's state tracking considerably. Edge-triggered mode would encourage fully draining each descriptor when it fires, which would give better throughput per wakeup, but would introduce starvation. If a descriptor is completely drained on each wakeup, a single high-throughput connection can monopolize event loop iteration, starving other sessions waiting for the same epoll_wait() batch. The choice of level-triggered events with fixed-size reads naturally enforces fairness by reading one chunk per iteration and yielding.

After reading data from a socket it is not guaranteed that we will be able to send the data as the kernel send buffer might not be able to store all of it. Two approaches seem viable. We could check the kernel send buffer capacity before reading and read only as much as can fit. The issue is that there is no way to query how much space is available in the kernel buffer before calling send() and the syscall only tells us the amount of bytes actually written. Another solution could be to only read when it is guaranteed that we can write the bytes, but epoll() events only notify about possibility to send at least one byte using send(). In both of these situations the core issue is that we cannot know whether if we read some data from the socket it will be possible to forward it. The data cannot be lost which means that it must be stored in memory owned by the proxy itself.

In the case where the throughput in a session is asymmetric it is possible that the internal buffers will keep accumulating data. Unbounded buffers can lead to memory pressure and in extreme cases OOM crash of the entire process tearing down all the sessions. Therefore it is crucial to bound the buffers, but this introduces the issue of what to do when the buffer is full and more data arrives.

This leads to the concept of backpressure, which is a flow control mechanism where a consumer signals to a producer to slow down when it cannot keep up with the rate of incoming data. The proxy uses TCP flow control mechanism to implement backpressure. After the buffer is full it stops reading data from the socket. As more data is received by the kernel eventually the kernel receive buffer will be full and TCP flow control window will close which will inform the producer at the transport layer that it must stop sending data. To prevent thrashing, which is repeated disabling and re-enabling EPOLLIN as the buffer oscillates around a single threshold, high and low watermarks are used. EPOLLIN is disabled when the buffer reaches the high watermark and only re-enabled once it drains to the low watermark. This ensures that a meaningful amount of capacity is available before reading resumes.

## Building the project

```
cmake -B build -G Ninja
cmake --build build
```
