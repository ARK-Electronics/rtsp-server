#!/bin/bash

# Setup XDG default paths
DEFAULT_XDG_CONF_HOME="$HOME/.config"
DEFAULT_XDG_DATA_HOME="$HOME/.local/share"
export XDG_CONFIG_HOME="${XDG_CONFIG_HOME:-$DEFAULT_XDG_CONF_HOME}"
export XDG_DATA_HOME="${XDG_DATA_HOME:-$DEFAULT_XDG_DATA_HOME}"
THIS_DIR="$(dirname "$(realpath "$BASH_SOURCE")")"

make

# Setup project directory
mkdir -p $XDG_DATA_HOME/rtsp-server/
cp $THIS_DIR/build/rtsp-server "${HOME}/.local/bin"
cp $THIS_DIR/config.toml $XDG_DATA_HOME/rtsp-server/

