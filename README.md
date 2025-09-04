# AudioReach Graph Manager

## Introduction

This repository hosts following software components for Linux:
- Audio Graph Manager (AGM)
- IPC client & server wrapper on top of DBus or Android HW binder
- TinyALSA PCM and Mixer plugins
- card-def parser

## Documentation

To be available soon.

## Build instructions

Graph Manager repository supports various build systems: Android, Autotools. Instructions to use Graph Manager on OpenEmbedded system will be updated soon.

##### Dependency on AudioReach Graph Service
Refer to instruction from [AudioReach Graph Service repository](https://github.com/Audioreach/audioreach-graphservices) to pull in Graph Service dependency

##### Configuration Options
- --with-syslog:  Use syslog message logging utliity. If target device is not Android, enable this option
- --with-glib:  Graph Manager uses string utilities which are not available in default C library on the target system. In such case, enable this option
- --with-no-ipc: If AGM is not running as a service, disable IPC communication to AGM

## License

Graph Manager is licensed under the BSD-3-Clause-Clear. Check out the [LICENSE](LICENSE) for more details
