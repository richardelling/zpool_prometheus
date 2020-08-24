# Prometheus metrics for ZFS pools
The _zpool_prometheus_ program produces 
[prometheus](https://github.com/prometheus)-compatible
metrics from zpools. In the UNIX tradition, _zpool_prometheus_
does one thing: read statistics from a pool and print them to
stdout. In many ways, this is a metrics-friendly output of 
statistics normally observed via the _zpool_ command.

## ZFS Versions
There are many implementations of ZFS on many OSes. The current
version is tested to work on:
* [ZFSonLinux](https://github.com/zfsonlinux/zfs) version 0.7 and later
* [cstor](https://github.com/openebs/cstor) userland ZFS for kubernetes

This should compile and run on other ZFS versions, though many 
do not have the latency histograms. Pull requests are welcome.

## Metrics Categories
The following metric types are collected:

| type | description | recurse? | zpool equivalent |
|---|---|---|---|
| zpool_stats | general size and data | yes | zpool list |
| zpool_scan_stats | scrub, rebuild, and resilver statistics | n/a | zpool status |
| zpool_latency | latency histograms for vdev | yes | zpool iostat -w |
| zpool_vdev | per-vdev stats, currently queues | no | zpool iostat -q | 
| zpool_req | per-vdev request size stats | yes | zpool iostat -r |

To be consistent with other prometheus collectors, each
metric has HELP and TYPE comments.

### Metric Names
Metric names are a mashup of:<br>
**\<type as above>\_\<ZFS internal name>\_\<units>**

For example, the pool's size metric is:<br>
**zpool_stats_size_bytes**

### Labels
The following labels are added to the metrics:

| label | metric | description |
|---|---|---|
| name | all | pool name |
| state | zpool_stats | pool state, as shown by _zpool status_ |
| state | zpool_scan_stats | scan state, as shown by _zpool status_ |
| vdev | zpool_stats, zpool_latency, zpool_vdev | vdev name |
| path | zpool_latency | device path name, if available |

#### vdev names
The vdev names represent the hierarchy of the pool configuration.
The top of the pool is "root" and the pool configuration follows 
beneath. A slash '/' is used to separate the levels.

For example, a simple pool with a single disk can have a `zpool status` of:
```

	NAME       STATE     READ WRITE CKSUM
	testpool   ONLINE       0     0     0
	  sdb      ONLINE       0     0     0
```
where the internal vdev hierarchy is:
```
root
root/disk-0
```
A more complex pool can have logs and redundancy. For example:
```
	NAME         STATE     READ WRITE CKSUM
	testpool     ONLINE       0     0     0
	  sda        ONLINE       0     0     0
	  sdb        ONLINE       0     0     0
	special	
	  mirror-2   ONLINE       0     0     0
	    sdc      ONLINE       0     0     0
	    sde      ONLINE       0     0     0
```
were the internal vdev hierarchy is:
```
root
root/disk-0
root/disk-1
root/mirror-2
root/mirror-2/disk-0
root/mirror-2/disk-1
```
Note that the special device does not carry a special description.
Log, cache, and spares are similarly not described in the hierarchy.

In some cases, the hierarchy can change over time. For example, if a 
vdev is removed, replaced, or attached then the hierarchy can grow or 
shrink as the vdevs come and go. Thus to determine the stats for a specific
physical device, use the `path`

#### path names
When a vdev has an associated path, then the path's name is placed
in the `path` value. For example:
```
path="/dev/sde1"
```
For brevity, the `zpool status` command often simplifies and truncates the
path name. Also, the `path` name can change upon reboot. 
Care should be taken to properly match the `path` of the desired device
when creating the pool or when querying in PromQL.

In an ideal world, the `devid` is a better direct method of uniquely 
identifying the device in Solaris-derived OSes. However, in Linux the 
`devid` is even less reliable than the `path`

### Values
Currently, [prometheus](https://github.com/prometheus) values must be
type float64. This is unfortunate because many ZFS metrics are 64-bit 
unsigned ints. When the actual metric values exceed the significant 
size of the floats (52 bits) then the value resets. This prevents problems
that occur due loss of resolution as the least significant bits are ignored
during the conversion to float64.

Pro tip: use PromQL rate(), irate() or some sort of non-negative derivative 
(influxdb or graphite) for these counters.

## Building
Building is simplified by using cmake.
It is as simple as possible, but no simpler.
By default, [ZFSonLinux](https://github.com/zfsonlinux/zfs) 
installs the necessary header and library files in _/usr/local_.
If you place those files elsewhere, then edit _CMakeLists.txt_ and
change the _INSTALL_DIR_
```bash
# generic ZFSonLinux build
cmake .
make
```

For Ubuntu, versions 16+ include ZFS packages, but not all are installed
by default. In particular, the required header files are in the
`libzfslinux-dev` package. This changes the process slightly:
```
# Ubuntu 16+ build
apt install libzfslinux-dev
mv CMakeLists.ubuntu.txt CMakeLists.txt
cmake .
make 
```

If successful, the _zpool_prometheus_ executable is created.

## Installing
Installation is left as an exercise for the reader because
there are many different methods that can be used.
Ultimately the method depends on how the local metrics collection is 
implemented and the local access policies.

There are two basic methods known to work:
1. Run a HTTP server that runs _zpool_prometheus_.
   A simple python+flask example server is included as _serve_zpool_prometheus.py_
2. Run a scheduled (eg cron) job that redirects the output
   to a file that is subsequently read by 
   [node_exporter](https://github.com/prometheus/node_exporter)

Helpful comments in the source code are available.

To install the _zpool_prometheus_ executable in _INSTALL_DIR_, use
```bash
make install
```

## Caveat Emptor
* Like the _zpool_ command, _zpool_prometheus_ takes a reader 
  lock on spa_config for each imported pool. If this lock blocks,
  then the command will also block indefinitely and might be
  unkillable. This is not a normal condition, but can occur if 
  there are bugs in the kernel modules. 
  For this reason, care should be taken:
  * avoid spawning many of these commands hoping that one might 
    finish
  * avoid frequent updates or short prometheus scrape time
    intervals, because the locks can interfere with the performance
    of other instances of _zpool_ or _zpool_prometheus_

* Metric values can overflow because the internal ZFS unsigned 64-bit
  int values do not transform to floats without loss of precision.

* Histogram sum values are always zero. This is because ZFS does
  not record that data currently. For most histogram uses this isn't
  a problem, but be aware of prometheus histogram queries that
  expect a non-zero histogram sum.

## Feedback Encouraged
Pull requests and issues are greatly appreciated. Visit
https://github.com/richardelling/zpool_prometheus
