"""Replace the embedded assembly manifest in-place by locating it from its content.

Rather than guessing MinGW's directory encoding, we find the manifest XML payload directly,
then walk backwards to the resource data entry that points at it (a 4-byte RVA equal to the
payload's RVA plus a matching size), and update that size field.
"""
import struct, sys, os

os.chdir(r'C:\Users\viz\Documents\Strata\Workspace\screenshot_mvp')


def load(exe):
    d = bytearray(open(exe, 'rb').read())
    pe = struct.unpack_from('<I', d, 0x3C)[0]
    nsec = struct.unpack_from('<H', d, pe + 6)[0]
    sizeopt = struct.unpack_from('<H', d, pe + 20)[0]
    magic = struct.unpack_from('<H', d, pe + 24)[0]
    opt = pe + 24
    dd_off = opt + (112 if magic == 0x20b else 96)
    res_rva, res_size = struct.unpack_from('<II', d, dd_off + 16)
    secs = []
    p = opt + sizeopt
    for i in range(nsec):
        nm = bytes(d[p:p + 8]).rstrip(b'\x00').decode(errors='replace')
        vsz, va, rsz, rptr = struct.unpack_from('<IIII', d, p + 8)
        secs.append(dict(name=nm, va=va, vsz=vsz, rsz=rsz, rptr=rptr, hdr=p))
        p += 40
    return d, pe, opt, dd_off, res_rva, res_size, secs


def o2r(secs, off):
    for s in secs:
        if s['rptr'] <= off < s['rptr'] + max(s['rsz'], s['vsz']):
            return s['va'] + (off - s['rptr'])
    return None


def sec_of(secs, off):
    for s in secs:
        if s['rptr'] <= off < s['rptr'] + max(s['rsz'], s['vsz']):
            return s
    return None


def replace(exe, manifest_path):
    new_xml = open(manifest_path, 'rb').read()
    d, pe, opt, dd_off, res_rva, res_size, secs = load(exe)

    rsrc = next((s for s in secs if s['va'] == res_rva), None)
    if rsrc is None:
        print('%s: no .rsrc section' % exe)
        return False

    lo, hi = rsrc['rptr'], rsrc['rptr'] + max(rsrc['rsz'], rsrc['vsz'])
    start = d.find(b'<?xml', lo, hi)
    if start < 0:
        print('%s: no XML payload inside .rsrc' % exe)
        return False

    # The manifest runs to its closing </assembly> tag.
    endtag = d.find(b'</assembly>', start)
    if endtag < 0:
        print('%s: manifest has no closing tag' % exe)
        return False
    old_end = endtag + len(b'</assembly>')
    # Trim trailing newline the resource compiler may have appended.
    while old_end < hi and d[old_end] in (0x0a, 0x0d, 0x00):
        old_end += 1
    old_size = old_end - start

    payload_rva = o2r(secs, start)
    if payload_rva is None:
        print('%s: could not map payload offset to RVA' % exe)
        return False

    # Find the resource data entry (RVA,size pair) that points at this payload.
    size_field = None
    q = lo
    while q + 8 <= hi:
        rva, sz = struct.unpack_from('<II', d, q)
        if rva == payload_rva and sz == old_size:
            size_field = q + 4
            break
        q += 1
    if size_field is None:
        # fall back: any entry pointing at the payload RVA
        q = lo
        while q + 8 <= hi:
            rva, sz = struct.unpack_from('<II', d, q)
            if rva == payload_rva:
                size_field = q + 4
                old_size = sz
                break
            q += 1
    if size_field is None:
        print('%s: resource entry for manifest not found (payloadRVA=%s oldsize=%d)'
              % (exe, hex(payload_rva), old_size))
        return False

    room = hi - start
    if len(new_xml) > room:
        print('%s: new manifest (%d bytes) exceeds available room (%d)' % (exe, len(new_xml), room))
        return False

    d[start:start + old_size] = new_xml
    if len(new_xml) < old_size:
        d[start + len(new_xml):start + old_size] = b'\x00' * (old_size - len(new_xml))
    struct.pack_into('<I', d, size_field, len(new_xml))

    # Grow section / directory sizes if the payload now ends further out.
    end_in_sec = start + len(new_xml) - rsrc['rptr']
    if end_in_sec > rsrc['vsz']:
        struct.pack_into('<I', d, rsrc['hdr'] + 8, ((end_in_sec + 0x1FF) // 0x200) * 0x200)
        struct.pack_into('<I', d, rsrc['hdr'] + 12, end_in_sec)
    if end_in_sec > res_size:
        struct.pack_into('<I', d, dd_off + 20, end_in_sec)

    open(exe, 'wb').write(bytes(d))
    print('%s: manifest replaced at file offset %d (rva %s), %d -> %d bytes'
          % (exe, start, hex(payload_rva), old_size, len(new_xml)))
    return True


if __name__ == '__main__':
    ok = all(replace(e, 'dpi.manifest') for e in sys.argv[1:])
    sys.exit(0 if ok else 1)
