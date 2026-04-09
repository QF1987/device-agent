# device-agent

DeviceOps cross-platform device agent written in C++.

Part of the [DeviceOps](https://github.com/QF1987/terminal-agent) ecosystem — the device-side agent that connects physical terminals to the DeviceOps management plane.

## What it does

- **Heartbeat**: Periodic liveness signals to the management server
- **Status Reporting**: Full device state snapshots on a configurable interval
- **Event Reporting**: Real-time fault/transaction/event notifications
- **Command Stream**: Long-lived connection receiving commands from the server (reboot, config push, OTA, etc.)
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
brew install cmake grpc protobuf

# or install vcpkg and:
vcpkg install grpc protobuf
```

### Build

```bash
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j$(nproc)
```

Output: `build/bin/device-agent`

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

- ✅ macOS (Apple Silicon & Intel)
- ✅ Linux (x86_64, ARM64, ARMv7)
- ✅ Windows (via CMake)
- ✅ Embedded Linux (Buildroot, Yocto)
- ✅ Android (NDK)
- ✅ iOS (via C++ framework)

## License

Apache 2.0
