#!/bin/bash
# Wrapper per GDB che forza debuginfod off all'avvio
exec /usr/bin/gdb -iex "set debuginfod enabled off" "$@"
