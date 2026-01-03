#!/usr/bin/env python3
"""
NTP Client Test Script for Caesium GPS Time Server

Tests the NTP server and calculates the time offset between
the server and local system clock.

Usage:
    python3 test_ntp.py [hostname] [--verbose]

Example:
    python3 test_ntp.py caesium.ubnt.local

Note, this is just a test script, the `sntp` is probably
a better choice for testing.
"""

import socket
import struct
import sys
import time

# NTP epoch starts 1900-01-01, Unix epoch starts 1970-01-01
# Difference in seconds: 70 years worth
NTP_UNIX_DELTA = 2208988800

# NTP packet format (48 bytes)
# Byte 0: LI (2 bits) | VN (3 bits) | Mode (3 bits)
# Byte 1: Stratum
# Byte 2: Poll interval
# Byte 3: Precision
# Bytes 4-7: Root delay
# Bytes 8-11: Root dispersion  
# Bytes 12-15: Reference ID
# Bytes 16-23: Reference timestamp
# Bytes 24-31: Originate timestamp
# Bytes 32-39: Receive timestamp
# Bytes 40-47: Transmit timestamp

NTP_PORT = 123
NTP_PACKET_FORMAT = "!B B B b I I 4s Q Q Q Q"
NTP_PACKET_SIZE = 48


def ntp_to_unix(ntp_time: int) -> float:
    """Convert NTP 64-bit timestamp to Unix float timestamp."""
    # NTP timestamp: upper 32 bits = seconds, lower 32 bits = fraction
    seconds = (ntp_time >> 32) & 0xFFFFFFFF
    fraction = ntp_time & 0xFFFFFFFF
    
    # Convert fraction to decimal (fraction / 2^32)
    unix_seconds = seconds - NTP_UNIX_DELTA
    unix_fraction = fraction / (2**32)
    
    return unix_seconds + unix_fraction


def unix_to_ntp(unix_time: float) -> int:
    """Convert Unix float timestamp to NTP 64-bit timestamp."""
    seconds = int(unix_time) + NTP_UNIX_DELTA
    fraction = int((unix_time % 1) * (2**32))
    return (seconds << 32) | fraction


def make_ntp_request() -> tuple[bytes, float]:
    """
    Create an NTP client request packet.
    Returns (packet_bytes, transmit_timestamp_unix).
    """
    # LI=0 (no warning), VN=4 (NTPv4), Mode=3 (client)
    li_vn_mode = (0 << 6) | (4 << 3) | 3
    
    # Capture transmit time as late as possible
    t1 = time.time()
    transmit_ts = unix_to_ntp(t1)
    
    # Pack the request - most fields are 0 for client request
    packet = struct.pack(
        "!B B B b I I 4s Q Q Q Q",
        li_vn_mode,     # LI/VN/Mode
        0,              # Stratum (0 = unspecified for client)
        0,              # Poll interval
        0,              # Precision
        0,              # Root delay
        0,              # Root dispersion
        b'\x00\x00\x00\x00',  # Reference ID
        0,              # Reference timestamp
        0,              # Originate timestamp
        0,              # Receive timestamp
        transmit_ts,    # Transmit timestamp (T1)
    )
    
    return packet, t1


def parse_ntp_response(data: bytes) -> dict:
    """Parse an NTP response packet."""
    if len(data) < NTP_PACKET_SIZE:
        raise ValueError(f"Packet too short: {len(data)} bytes")
    
    unpacked = struct.unpack("!B B B b I I 4s Q Q Q Q", data[:48])
    
    li_vn_mode = unpacked[0]
    li = (li_vn_mode >> 6) & 0x3
    vn = (li_vn_mode >> 3) & 0x7
    mode = li_vn_mode & 0x7
    
    stratum = unpacked[1]
    poll = unpacked[2]
    precision = unpacked[3]
    root_delay = unpacked[4]
    root_dispersion = unpacked[5]
    ref_id = unpacked[6]
    ref_ts = unpacked[7]
    orig_ts = unpacked[8]   # T1 - our transmit time, echoed back
    rx_ts = unpacked[9]     # T2 - server receive time
    tx_ts = unpacked[10]    # T3 - server transmit time
    
    return {
        'li': li,
        'version': vn,
        'mode': mode,
        'stratum': stratum,
        'poll': poll,
        'precision': precision,
        'precision_sec': 2 ** precision if precision < 0 else precision,
        'root_delay': root_delay / (2**16),  # Convert from NTP short format
        'root_dispersion': root_dispersion / (2**16),
        'ref_id': ref_id,
        'ref_id_str': ref_id.decode('ascii', errors='replace').rstrip('\x00'),
        'reference_ts': ref_ts,
        'originate_ts': orig_ts,
        'receive_ts': rx_ts,
        'transmit_ts': tx_ts,
        # Convert to Unix timestamps
        'originate_unix': ntp_to_unix(orig_ts) if orig_ts else 0,
        'receive_unix': ntp_to_unix(rx_ts) if rx_ts else 0,
        'transmit_unix': ntp_to_unix(tx_ts) if tx_ts else 0,
    }


def stratum_name(stratum: int) -> str:
    """Get human-readable stratum description."""
    if stratum == 0:
        return "unspecified/KoD"
    elif stratum == 1:
        return "primary (GPS/atomic)"
    elif stratum <= 15:
        return f"secondary (stratum {stratum})"
    else:
        return "unsynchronized"


def li_name(li: int) -> str:
    """Get leap indicator description."""
    names = {
        0: "no warning",
        1: "+1 second (leap second insert)",
        2: "-1 second (leap second delete)",
        3: "UNSYNCHRONIZED",
    }
    return names.get(li, "unknown")


def query_ntp_server(host: str, port: int = NTP_PORT, timeout: float = 5.0, verbose: bool = False) -> dict:
    """
    Query an NTP server and calculate time offset.
    
    Returns dict with offset, delay, and server info.
    """
    # Create UDP socket
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.settimeout(timeout)
    
    try:
        # Resolve hostname
        if verbose:
            print(f"Resolving {host}...")
        addr = socket.gethostbyname(host)
        if verbose:
            print(f"Resolved to {addr}")
        
        # Create and send request
        request, t1 = make_ntp_request()
        
        if verbose:
            print(f"Sending NTP request (T1 = {t1:.6f})...")
        
        sock.sendto(request, (addr, port))
        
        # Receive response and capture arrival time immediately
        data, server_addr = sock.recvfrom(1024)
        t4 = time.time()  # T4 - local time when response arrived
        
        if verbose:
            print(f"Received {len(data)} bytes from {server_addr}")
            print(f"Response arrival (T4 = {t4:.6f})")
        
        # Parse response
        response = parse_ntp_response(data)
        
        # Extract timestamps for offset calculation
        # T1 = client transmit time (we sent)
        # T2 = server receive time (from response)
        # T3 = server transmit time (from response)  
        # T4 = client receive time (we captured)
        
        t2 = response['receive_unix']
        t3 = response['transmit_unix']
        
        # Calculate round-trip delay and clock offset
        # delay = (T4 - T1) - (T3 - T2)
        # offset = ((T2 - T1) + (T3 - T4)) / 2
        
        delay = (t4 - t1) - (t3 - t2)
        offset = ((t2 - t1) + (t3 - t4)) / 2
        
        return {
            'success': True,
            'host': host,
            'address': addr,
            't1': t1,
            't2': t2,
            't3': t3,
            't4': t4,
            'offset': offset,
            'delay': delay,
            'response': response,
        }
        
    except socket.timeout:
        return {'success': False, 'error': 'Timeout waiting for response'}
    except socket.gaierror as e:
        return {'success': False, 'error': f'DNS resolution failed: {e}'}
    except Exception as e:
        return {'success': False, 'error': str(e)}
    finally:
        sock.close()


def format_time_offset(offset_sec: float) -> str:
    """Format a time offset nicely."""
    abs_offset = abs(offset_sec)
    sign = "+" if offset_sec >= 0 else "-"
    
    if abs_offset < 0.001:
        return f"{sign}{abs_offset * 1_000_000:.1f} µs"
    elif abs_offset < 1:
        return f"{sign}{abs_offset * 1000:.3f} ms"
    else:
        return f"{sign}{abs_offset:.6f} s"


def main():
    # Parse arguments
    host = "caesium.ubnt.local"
    verbose = False
    
    for arg in sys.argv[1:]:
        if arg in ('-v', '--verbose'):
            verbose = True
        elif not arg.startswith('-'):
            host = arg
    
    print(f"╔════════════════════════════════════════════════════════════╗")
    print(f"║           Caesium NTP Server Test                          ║")
    print(f"╚════════════════════════════════════════════════════════════╝")
    print()
    print(f"Target: {host}:{NTP_PORT}")
    print()
    
    # Query the server
    result = query_ntp_server(host, verbose=verbose)
    
    if not result['success']:
        print(f"❌ Error: {result['error']}")
        sys.exit(1)
    
    resp = result['response']
    
    # Check for unsynchronized server
    if resp['li'] == 3 or resp['stratum'] == 0:
        print("⚠️  Warning: Server reports UNSYNCHRONIZED (GPS not locked?)")
        print()
    
    # Server info
    print("┌─ Server Info ─────────────────────────────────────────────┐")
    print(f"│ Address:     {result['address']}")
    print(f"│ Stratum:     {resp['stratum']} ({stratum_name(resp['stratum'])})")
    print(f"│ Reference:   {resp['ref_id_str']!r}")
    print(f"│ Version:     NTPv{resp['version']}")
    print(f"│ Leap:        {li_name(resp['li'])}")
    print(f"│ Precision:   2^{resp['precision']} = {resp['precision_sec']:.9f} sec")
    print("└────────────────────────────────────────────────────────────┘")
    print()
    
    # Timestamps
    if verbose:
        print("┌─ Timestamps ──────────────────────────────────────────────┐")
        print(f"│ T1 (client tx):  {result['t1']:.6f}")
        print(f"│ T2 (server rx):  {result['t2']:.6f}")
        print(f"│ T3 (server tx):  {result['t3']:.6f}")
        print(f"│ T4 (client rx):  {result['t4']:.6f}")
        print("└────────────────────────────────────────────────────────────┘")
        print()
    
    # Results
    print("┌─ Synchronization Results ────────────────────────────────┐")
    print(f"│ Round-trip delay:  {format_time_offset(result['delay']):>12}")
    print(f"│ Clock offset:      {format_time_offset(result['offset']):>12}")
    print("└────────────────────────────────────────────────────────────┘")
    print()
    
    # Epoch times (informational)
    local_epoch = result['t4']  # Local time when we received response
    server_epoch = result['t3']  # Server time when it sent response
    print("┌─ Epoch Times (informational) ────────────────────────────┐")
    print(f"│ Local system: {local_epoch:.6f}")
    print(f"│ NTP server:   {server_epoch:.6f}")
    print("└────────────────────────────────────────────────────────────┘")
    print()
    
    # Interpretation
    offset_ms = result['offset'] * 1000
    if abs(offset_ms) < 1:
        print("✅ Excellent! Your clock is within 1ms of the GPS time server.")
    elif abs(offset_ms) < 10:
        print("✅ Good! Your clock is within 10ms of the GPS time server.")
    elif abs(offset_ms) < 100:
        print("⚠️  Your clock is off by more than 10ms from GPS time.")
    else:
        print("❌ Significant offset detected! Consider syncing your system clock.")
    
    if result['offset'] > 0:
        print(f"   Your clock is {format_time_offset(abs(result['offset']))} BEHIND the server.")
    else:
        print(f"   Your clock is {format_time_offset(abs(result['offset']))} AHEAD of the server.")


if __name__ == "__main__":
    main()
