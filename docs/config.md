# Upstream configuration

Orbit supports load balancing connections over multiple upstreams. Upstreams can be configured through command line arguments
like so:

```
--upstream pair1 --upstream pair2
```

where pairs are either DNS/IPv4 HOST:PORT or IPv6 [ADDRESS]:PORT.

Environment variable ORBIT_UPSTREAMS can also be used to pass a comma-separated
list of pairs like so:

```
ORBIT_UPSTREAMS=pair1,pair2
```

Orbit configuration requires at least one upstream to be configured using either of the above methods.