#!/bin/bash
echo "Test Message" | nc -u -w1 esp32-ethernet.ubnt.local 4210
