#!/usr/bin/env python3
"""Modbus TCP masters. Used as a functional battery and as a concurrent soak.

    python3 tools/tcp_master.py --port 5020
    python3 tools/tcp_master.py --port 5020 --clients 100 --txns 200
"""
import argparse
import socket
import struct
import sys
import threading
import time

MBAP = struct.Struct(">HHHB")


def build(txn, unit, pdu: bytes) -> bytes:
    return MBAP.pack(txn, 0, len(pdu) + 1, unit) + pdu


def read_holding(txn, unit, start, qty) -> bytes:
    return build(txn, unit, struct.pack(">BHH", 0x03, start, qty))


def recv_exact(sock, n):
    buf = b""
    while len(buf) < n:
        chunk = sock.recv(n - len(buf))
        if not chunk:
            raise ConnectionError("peer closed")
        buf += chunk
    return buf


def recv_adu(sock):
    head = recv_exact(sock, 6)
    txn, proto, length = struct.unpack(">HHH", head)
    rest = recv_exact(sock, length)
    return txn, proto, rest[0], rest[1:]


def battery(port):
    npass = nfail = 0

    def case(name, ok, detail=""):
        nonlocal npass, nfail
        print(f"  {'PASS' if ok else 'FAIL'}  {name:<44} {detail}")
        if ok:
            npass += 1
        else:
            nfail += 1

    s = socket.create_connection(("127.0.0.1", port), timeout=5)
    s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)

    s.sendall(read_holding(0x0001, 17, 0, 2))
    txn, proto, unit, pdu = recv_adu(s)
    case("read 2 registers", txn == 1 and unit == 17 and pdu[0] == 0x03,
         f"txn={txn} unit={unit} pdu={pdu.hex(' ')}")
    # Backend-independent addressing check. The first version of this asserted
    # a literal 0x1100, which is InProcessRtu's seeding; against Project 1's
    # slave (seeded 0x1000 + i) it failed. The value is not the point --
    # what matters is that consecutive registers return consecutive values,
    # which proves the read reached the right address and not merely that
    # something replied.
    r0 = (pdu[2] << 8) | pdu[3]
    r1 = (pdu[4] << 8) | pdu[5]
    case("consecutive registers read consecutive values", r1 - r0 == 1,
         f"reg0=0x{r0:04X} reg1=0x{r1:04X}")

    s.sendall(read_holding(0xBEEF, 17, 0, 1))
    txn, _, _, _ = recv_adu(s)
    case("transaction id echoed exactly", txn == 0xBEEF, f"0x{txn:04X}")

    # pipelining: three requests in one segment, responses must come back in
    # order with their own transaction ids
    pipeline = b"".join(read_holding(0x100 + i, 17, i, 1) for i in range(3))
    s.sendall(pipeline)
    got = [recv_adu(s)[0] for _ in range(3)]
    case("three pipelined requests answered in order",
         got == [0x100, 0x101, 0x102], str([hex(g) for g in got]))

    # split a single request across two segments
    frame = read_holding(0x200, 17, 5, 1)
    s.sendall(frame[:4])
    time.sleep(0.05)
    s.sendall(frame[4:])
    txn, _, _, pdu = recv_adu(s)
    case("request split across two segments reassembled",
         txn == 0x200 and pdu[0] == 0x03, f"txn=0x{txn:04X}")

    # absent unit -> gateway exception 0x0B
    s.sendall(read_holding(0x300, 99, 0, 1))
    txn, _, _, pdu = recv_adu(s)
    case("absent unit -> exception 0x0B",
         txn == 0x300 and pdu[0] == 0x83 and pdu[1] == 0x0B, pdu.hex(' '))

    # slave-level exception must pass through unchanged
    s.sendall(read_holding(0x400, 17, 0, 0xFFFF))
    txn, _, _, pdu = recv_adu(s)
    case("slave exception 0x03 forwarded", pdu[0] == 0x83 and pdu[1] == 0x03,
         pdu.hex(' '))

    s.close()
    return npass, nfail


def soak(port, clients, txns):
    errors = [0] * clients
    mismatches = [0] * clients
    done = [0] * clients

    def worker(idx):
        try:
            s = socket.create_connection(("127.0.0.1", port), timeout=10)
            s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
            for i in range(txns):
                txn = (idx << 8 | (i & 0xFF)) & 0xFFFF
                reg = i % 60
                s.sendall(read_holding(txn, 17, reg, 1))
                rtxn, _, runit, pdu = recv_adu(s)
                if rtxn != txn or runit != 17:
                    mismatches[idx] += 1
                elif pdu[0] == 0x03 and len(pdu) == 4:
                    want = (17 << 8) | reg
                    got = (pdu[2] << 8) | pdu[3]
                    if got != want:
                        mismatches[idx] += 1
                done[idx] += 1
            s.close()
        except Exception:
            errors[idx] += 1

    t0 = time.time()
    threads = [threading.Thread(target=worker, args=(i,)) for i in range(clients)]
    for t in threads:
        t.start()
    for t in threads:
        t.join()
    dt = time.time() - t0

    total = sum(done)
    print(f"\n  {clients} concurrent masters x {txns} transactions")
    print(f"  completed .............. {total}")
    print(f"  connection errors ...... {sum(errors)}")
    print(f"  misrouted responses .... {sum(mismatches)}")
    print(f"  elapsed ................ {dt:.2f} s  ({total/dt:.0f} txn/s)")
    return sum(mismatches) == 0 and sum(errors) == 0 and total == clients * txns


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=5020)
    ap.add_argument("--clients", type=int, default=0)
    ap.add_argument("--txns", type=int, default=200)
    a = ap.parse_args()

    if a.clients:
        ok = soak(a.port, a.clients, a.txns)
        print("\n  " + ("PASS  no misrouted responses" if ok else "FAIL"))
        return 0 if ok else 1

    print("\n=== Modbus TCP functional battery ===")
    npass, nfail = battery(a.port)
    print(f"\n  {npass} passed, {nfail} failed\n")
    return 0 if nfail == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
