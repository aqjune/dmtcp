## Installing DMTCP

### Prerequisites

On debian, you need to install some prerequisties:

```
sudo apt install git gcc g++ make -y
```

And install python3 if you need the tests:

```
sudo apt install python3 -y
```

On macOS, install the Xcode command-line tools and Python 3:

```bash
xcode-select --install
brew install autoconf automake python
```

macOS builds select `--enable-control-plane-only` automatically. This builds
`dmtcp_coordinator`, `dmtcp_command`, and an experimental registration-only
`dmtcp_launch`. It does not build `dmtcp_restart`, the checkpoint runtime,
MTCP, or plugins, and it cannot checkpoint macOS applications.

Start a coordinator, then launch a user-owned, non-hardened executable:

```bash
./bin/dmtcp_coordinator
./bin/dmtcp_launch --join-coordinator -- ./my-program
```

The process appears in `dmtcp_command --status` and responds to coordinator
kill and quit commands. Checkpoint commands fail explicitly without stopping
the process. Registration-only mode rejects scripts, process creation, and
image replacement (`exec`) because it cannot yet track additional or replaced
processes. The launcher uses `DYLD_INSERT_LIBRARIES`; macOS strips this
variable for platform binaries and can reject it for hardened applications.
Do not disable System Integrity Protection or hardened-runtime protections to
use this feature.

The coordinator listens on `127.0.0.1` by default. To accept workers from
other hosts, bind an intended interface explicitly, for example:

```bash
./bin/dmtcp_coordinator --coord-bind 192.0.2.10
```

Do not use `--coord-bind 0.0.0.0` on an untrusted network; the DMTCP wire
protocol does not authenticate clients.

Run the portable control-plane tests with:

```bash
make check-control
```

### Build instructions

To build, use:
```bash
  ./configure
  make
  # [Optional]
  make check
```

As usual, you may prefer to use `make` with `'-j'` for faster, parallel builds.

This software runs in the original directory, and
```bash
  make install
```
will install to the install directory based on configure.

Distribution packages for Debian/Ubuntu, OpenSUSE and Fedora are available in their respective repositories.

### Multilib support
To support multilib (mixed use of 32- and 64-bit applications),
there are two possibilities:
1.  Install both the 32- and 64-bit versions of DMTCP packages for your distro.
OR:
2.  Build from source:
```bash
  ./configure
  make
  # Do NOT do 'make clean' here.  You will lose bin/dmtcp_launch, etc.
  ./configure --enable-m32
  make clean   # This won't remove the 64-bit binary and library files.
  make
  # Optionally, change 'make' to 'make install'' if desired.
```

When running DMTCP with multilib support, the 64-bit applications,
`dmtcp_launch`, `dmtcp_restart`, and `dmtcp_command`
all inter-operate with both 32- and 64-bit applications.

If your process will create multiple processes, you should use standard DMTCP.
A brief overview follows.
The general methodology is:
```bash
  ./dmtcp_coordinator #in one window: type h to the coordinator to see commands
  ./dmtcp_launch a.out <args,...>
```

Note that DMTCP creates log files under
`/tmp/${USER}/dmtcp-${USER}@${HOST}/*.log` by default. Set
`DMTCP_LOG_LEVEL=trace` before launching an application to include trace-level
logs.

See the file [QUICK-START.md](QUICK-START.md) for further information on using DMTCP.


## Running without a separate coordinator process:

Usually, the most convenient procedure is to use DMTCP in its
default mode, which includes a separate DMTCP coordinator process.
In some rare cases, you may wish to avoid a separate coordinator process by
providing a `--no-coordinator` flag to `dmtcp_launch`. Multiprocess
computations are not supported with this flag.
