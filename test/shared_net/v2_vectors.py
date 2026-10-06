#!/usr/bin/env python3
"""网络 v2 头/目录的独立大端布局和 CRC 检查。"""
import struct


def be(fmt, *values):
    return struct.pack('>' + fmt, *values)


def crc32c(blob):
    value = 0xffffffff
    for byte in blob:
        value ^= byte
        for _ in range(8):
            value = (value >> 1) ^ (0x82f63b78 if value & 1 else 0)
    return value ^ 0xffffffff


def packet():
    scope = b'DZS2' + be('IQ', 1, 7) + bytes(range(16))
    source, publisher, target = bytes(range(16)), bytes(range(16, 32)), bytes(range(32, 48))
    payload = b'v2'
    result = bytearray(176 + len(payload))
    result[0:4] = b'DZMX'; result[4] = 2; result[5] = 1
    result[8:10] = be('H', 176); result[10:12] = be('H', len(payload)); result[12:40] = scope[4:]
    result[40:56] = source; result[56:64] = be('Q', 11); result[64:80] = publisher; result[80:88] = be('Q', 13)
    result[88:104] = target; result[104:112] = be('Q', 17)
    result[112:116] = be('I', len(payload)); result[116:120] = be('I', 0); result[120:124] = be('I', 1)
    result[124:128] = be('I', crc32c(payload)); result[128:132] = be('I', 71); result[132:136] = be('I', 0)
    result[136:144] = be('Q', 19); result[148] = 1; result[149] = 0
    result[160:168] = be('Q', 101); result[168:176] = be('Q', 202); result[176:] = payload
    result[144:148] = be('I', crc32c(result))
    assert len(result) == 178
    return bytes(result)


def directory():
    topic = b'/v2/topic'
    scope = b'DZS2' + be('IQ', 1, 0) + bytes(range(16))
    entry = scope + be('IIQHHHHQ', 71, 0, 9, len(topic), 3, 31001, 1, 17) + topic
    body = be('I', 1) + entry
    assert len(entry) == 64 + len(topic)
    return body


if __name__ == '__main__':
    assert crc32c(b'123456789') == 0xe3069283
    assert len(packet()) == 178
    assert len(directory()) == 4 + 64 + len(b'/v2/topic')
    print('v2 independent vectors passed')
