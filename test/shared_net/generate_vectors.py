#!/usr/bin/env python3
"""独立 Python struct/CRC 参考向量；--check 不修改制品。"""
import argparse
import json
from pathlib import Path
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


def packet(blob, offset):
    result = bytearray(blob)
    result[offset:offset + 4] = b'\0' * 4
    result[offset:offset + 4] = be('I', crc32c(result))
    return bytes(result)


def fnv64(blob):
    value = 14695981039346656037
    for byte in blob:
        value = ((value ^ byte) * 1099511628211) & ((1 << 64) - 1)
    return value


def vectors():
    topic = b'/golden/topic'
    domain = 0x0102030405060708
    key = f'DZSC2:1:{domain}:{len(topic)}:'.encode() + topic
    fingerprint = be('QQ', fnv64(key), fnv64(b'identity:' + key))
    scope = b'DZS2' + be('IQ', 1, domain) + fingerprint
    msg_id = 0x11223344
    src, pub, dst = bytes(range(16)), bytes(range(16, 32)), bytes(range(32, 48))
    se, te, seq, re = 0x1112131415161718, 0x2122232425262728, 0x3132333435363738, 0x4142434445464748
    blob = bytes([0x7b]) * 1025
    def mx(kind, payload, index=0):
        h = b'DZMX' + be('BBHHHIQ', 1, kind, 0, 160, len(payload), 1, domain) + fingerprint
        h += src + be('Q', se) + pub + be('Q', seq) + dst + be('Q', te)
        h += be('IIIIIIQI', len(blob), index, 2, crc32c(blob), msg_id, 0, re, 0)
        h += be('BBHQ', 1, 1, 0, 0)
        assert len(h) == 160
        return packet(h + payload, 144)
    def descriptor(role=0, epoch=0):
        return scope + be('IIQHH', msg_id, 0, epoch, len(topic), role) + topic
    sid, request = 0x5152535455565758, 0x6162636465666768
    tx_name = f'dzgw_tx_v2_{src.hex()}_{se:016x}_{sid:016x}'.encode()
    def lc(kind, body, req=request, session=sid, epoch=se):
        return b'DZLC' + be('HHIIQQQ', 2, kind, 40 + len(body), 0, req, session, epoch) + body
    result = {
        'route_key': scope + be('I', msg_id), 'crc_standard': be('I', crc32c(b'123456789')),
        'dzmx_data': mx(1, b'\x7b', 1), 'dzmx_ack': mx(2, b''),
        'dzmx_nack': mx(3, be('II', 0, 1)), 'dzmx_reject': mx(4, be('I', 3)),
        'directory_empty': be('I', 0), 'directory_pub': be('I', 1) + descriptor(1),
        'directory_sub': be('I', 1) + descriptor(2, re), 'directory_both': be('I', 1) + descriptor(3, re),
    }
    result['dzgd_hello'] = packet(b'DZGD' + be('BBH', 1, 1, 64) + src + be('QHHHHQIIQ', se, 24000, 4, 24004, 1, 7, 16777216, 0, 0), 52)
    def gc(kind, body, count=0, crc=0):
        return packet(b'DZGC' + be('BBHHH', 1, kind, 84, len(body), 0) + src + be('Q', se) + dst +
                      be('QQIIII', te, 7, 0, count, crc, 0) + body, 80)
    result['dzgc_request'] = gc(1, b'')
    result['dzgc_page'] = gc(2, result['directory_pub'], 1, crc32c(result['directory_pub']))
    result['dzgc_empty_page'] = gc(2, result['directory_empty'], 1, crc32c(result['directory_empty']))
    bodies = {
        1: src + be('QQI', 123, 456, 1),
        2: src + be('IIIQQH', 16777216, 33554432, 256, 1048576, 16, len(tx_name)) + tx_name,
        3: b'', 4: b'', 5: pub + descriptor(), 6: pub, 7: pub + descriptor(),
        8: pub + be('IQ', 9, 0), 9: pub + be('I', 9), 10: pub + be('Q', re),
        11: pub + be('B', 1), 12: pub, 13: be('B', 2) + scope + be('I', msg_id) + dst,
        14: be('I', 2) + b'{}', 17: pub + be('QIIII', seq, 3, 2, 2, 0),
        18: be('Q', 123), 19: be('Q', 123), 20: be('IH', 3, 3) + b'err',
        21: be('QQ', 9, 33554432), 22: pub + be('QII', 7, 2, 1),
        23: be('II', 1024, 1), 24: be('QQ', 67108864, 1024),
    }
    for kind, body in bodies.items():
        result[f'dzlc_{kind:02}'] = lc(kind, body, 0 if kind in (21, 22) else request,
                                      0 if kind == 1 else sid, 0 if kind == 1 else se)
    for kind in (0, 1):
        h = b'DZTX' + be('HHQQ', 2, 112, se, sid) + pub + be('Q', seq) + scope
        h += be('IIIBBHQQ', msg_id, len(blob), 0, 1, kind, 0, request if kind else 0, 987654321 if kind else 0)
        assert len(h) == 112
        result['dztx_reliable' if kind else 'dztx_best_effort'] = h + blob
    fields = {'topic': topic.decode(), 'domain': domain, 'msg_id': msg_id, 'source_epoch': se,
              'target_epoch': te, 'sequence': seq, 'route_epoch': re, 'session_id': sid, 'request_id': request,
              'scope_hex': scope.hex(), 'route_hash': fnv64(result['route_key'])}
    return {'description': '按执行方案独立生成的网络序参考向量', 'fields': fields,
            'vectors': {key: value.hex() for key, value in result.items()}}


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--check', action='store_true')
    args = parser.parse_args()
    target = Path(__file__).with_name('golden_vectors.json')
    expected = json.dumps(vectors(), ensure_ascii=False, indent=2) + '\n'
    if args.check:
        if target.read_text() != expected:
            raise SystemExit('参考向量与独立编码结果不一致')
        print('参考向量校验通过')
    else:
        target.write_text(expected)
