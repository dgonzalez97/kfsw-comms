# K-FSW Communications

K-FSW communications owns the common communication stacks, interfaces, and
routing lifecycle used by the application. Its first implementation integrates
[libcsp](https://github.com/libcsp/libcsp) for Cubesat Space Protocol routing.

The composition repository pins libcsp to commit
`097a039701c85e4ceb98e91f380810662e23878a` and supplies the target-specific
Kconfig values. The current host transport is libcsp's Zephyr USART driver with
KISS framing. CAN and flight routing tables are intentionally deferred.

## Packet ownership

K-FSW follows libcsp's zero-copy ownership rules directly:

- A packet returned by a CSP receive API belongs to the receiver until it is
  freed or passed to a CSP send/reply API.
- A packet passed to a CSP send API transfers ownership, including when the
  interface reports a transmit failure; callers must not reuse or free it.
- Interface receive paths transfer complete packets to CSP's router queue.
  libcsp either queues, routes, or frees them.
- Pool or queue exhaustion causes allocation failure or a counted packet drop.
  K-FSW-owned code does not retry indefinitely or allocate an unbounded queue.
