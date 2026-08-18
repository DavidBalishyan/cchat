#!/bin/bash

script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"

clear
go run "$script_dir/main.go" 127.0.0.1:8080
