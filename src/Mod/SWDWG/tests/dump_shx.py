import struct
path = r'C:\Program Files\Autodesk\AutoCAD 2016\UserDataCache\zh-cn\Support\ltypeshp.shx'
with open(path, 'rb') as f:
    data = f.read()
print('header:', data[:24])
first, last, count = struct.unpack('<3H', data[24:30])
print(f'first={first} last={last} count={count}')
tbl = data[30:30 + count * 4]
off = 30 + count * 4
for i in range(count):
    num, length = struct.unpack('<2H', tbl[i * 4:i * 4 + 4])
    name_end = data.find(b'\x00', off)
    name = data[off:name_end].decode('ascii', errors='replace')
    codes = data[name_end + 1:off + length]
    if name in ('ZIG', 'BAT', 'TRACK1', 'ZAG'):
        print(f'\n{name} num={num} len={length} raw={codes.hex()}')
        print('bytes:', ' '.join(f'{b:02X}' for b in codes))
    off += length
