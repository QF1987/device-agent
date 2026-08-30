# device-agent

DeviceOps cross-platform device agent written in C++.

Part of the [DeviceOps](https://github.com/QF1987/terminal-agent) ecosystem — the device-side agent that connects physical terminals to the DeviceOps management plane.

## What it does

- **Heartbeat**: Periodic liveness signals to the management server
- **Status Reporting**: Full device state snapshots on a configurable interval
- **Event Reporting**: Real-time fault/transaction/event notifications
- **Command Stream**: Long-lived connection receiving capability-gated commands from the server
- **Command Result Reporting**: Execution results sent back to the server

## Architecture

```
device-agent (this repo)
├── DeviceClient         — gRPC client wrapper (DeviceService + CommandService)
├── CommandHandler       — command dispatch and execution
├── Config               — JSON file / environment variable config
└── Logger               — structured logging (stdout + file)
```

## Build

### Prerequisites

```bash
# Ubuntu/Debian
sudo apt install cmake build-essential libgrpc++-dev libprotobuf-dev protobuf-compiler

# macOS
brew install cmake grpc protobuf abseil c-ares re2 openssl@3

# or install vcpkg and:
vcpkg install grpc protobuf
```

### Build

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --target device-agent
```

Output: `build/bin/device-agent`

The desktop CMake target uses `find_package(protobuf CONFIG)` and
`find_package(gRPC CONFIG)`, so Homebrew upgrades do not require editing
versioned Cellar paths by hand.

## Config

### JSON file (`/etc/device-agent/config.json`)

```json
{
  "device_id": "SH-PD-001",
  "token": "your-device-token-here",
  "server_host": "your-server.example.com",
  "server_port": 9090,
  "use_tls": false,
  "heartbeat_interval": 30,
  "heartbeat_timeout": 5,
  "status_interval": 300,
  "reconnect_base": 1,
  "reconnect_max": 60,
  "log_level": "info",
  "log_file": "/var/log/device-agent/agent.log"
}
```

### Environment variables

```bash
export DEVICE_ID="SH-PD-001"
export DEVICE_TOKEN="your-device-token-here"
export DEVICE_OPS_SERVER_HOST="your-server.example.com"
export DEVICE_OPS_SERVER_PORT=9090
export DEVICE_OPS_USE_TLS=0
export DEVICE_HEARTBEAT_INTERVAL=30
export LOG_LEVEL=info
```

## Run

```bash
# From JSON config file (default: /etc/device-agent/config.json)
./device-agent

# From specific config file
./device-agent -c /path/to/config.json

# From environment variables
./device-agent -e
```

## gRPC Services

- **DeviceService** (device → server): Heartbeat, ReportStatus, ReportEvent, ReportCommandResult
- **CommandService** (server → device): CommandStream (server-side streaming)

See the full protocol in `proto/terminal_agent/v1/`.

## Platform Support

Support is capability-specific; a platform build does not imply that every
command is available.

- ✅ Windows x64 — native build/executor, bootstrap service install, HTTP
  release download and real inventory/telemetry/network reporting. Firmware OTA
  is intentionally unsupported and Windows P2P download is not implemented.
- ✅ Android — NDK builds for arm64-v8a and armeabi-v7a, APK install and P2P
  release download. Inventory/telemetry parity with Windows is still pending.
- 🟡 Linux — desktop agent and P2P release core build on validated Linux targets;
  package installation, turnkey service deployment and full observability remain
  incomplete.
- 🟡 macOS — desktop agent and P2P code paths build, with only basic application
  handoff; full observability and managed installation/rollback remain incomplete.
- 🚧 iOS — no executor or build target.
- 🚧 Embedded Linux (Buildroot, Yocto) — not validated as a distinct target.

Firmware commands currently fail closed on every platform because no production
firmware adapter or target catalog is registered. Device gRPC transport also
remains plaintext; bootstrap HTTPS does not provide gRPC TLS/mTLS.

## License

Apache 2.0
