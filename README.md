# Open RealtimeAPI Embedded SDK

# Table of Contents

- [Installation](#installation)

## Platform/Device Support

This SDK has been developed tested on a `esp32s3`. You need INMP441 + MAX98357A hardware
to run this SDK.

## Installation

* `git submodule update --init --recursive`

Call `set-target` with the platform you are targetting. Today only `linux` and `esp32s3` are supported.
* `idf.py set-target esp32s3`

Set your Wifi SSID + Password as env variables
* `export WIFI_SSID=wifi_name`
* `export WIFI_PASSWORD=wifi_pass`
* `export OPENAI_API_KEY=token_value`

Or use the interactive helper script (must be sourced, not executed):
* `source ./setup-env.sh`


Build
* `idf.py build`

Flash & monitor
* `idf.py flash monitor`
